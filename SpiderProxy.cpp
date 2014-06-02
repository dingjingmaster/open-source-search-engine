#include "gb-include.h"

#include "Pages.h"
#include "TcpSocket.h"
#include "HttpServer.h"


//
// BASIC DETAILS
//
// . host #0 is solely responsible for testing the proxies and keeping
//   the results of the tests, using the user-defined test url which
//   each proxy downloads every 60 seconds or so.
// . host #0 also saves these stats to the spiderproxies.dat file
//   in the working dir (just an array of SpiderProxy instances)
// . any host needing a proxy server to use should ask host #0 for one
//   but if host #0 is dead then it should ask host #1, etc.
// . host #1 (etc.) will take over if it sees host #0 went dead
// . Conf::m_proxyIps (safebuf) holds space-sep'd list of proxyip:port pairs
// . since host #0 is responsible for giving you a proxy ip it can
//   do proxy load balancing for the whole cluster
// . TODO: to prevent host #0 from getting too slammed we can also recruit
//   other hosts to act just like host #0.

// host #0 breaks Conf::m_spiderIps safebuf into an array of
// SpiderProxy classes and saves to disk as spoderproxies.dat to ensure 
// persistence
class SpiderProxy {
public:
	// ip/port of the spider proxy
	long m_ip;
	short m_port;
	// last time we attempted to download the test url through this proxy
	long long m_lastDownloadTestAttemptMS;
	// use -1 to indicate timed out when downloading test url
	long   m_lastDownloadTookMS;
	// 0 means none... use mstrerror()
	long   m_lastDownloadError;
	// use -1 to indicate never
	long long m_lastSuccessfulTestMS;

	// how many times have we told a requesting host to use this proxy
	// to download their url with.
	long m_numDownloadRequests;

	// how many are outstanding? everytime a host requests a proxyip
	// it also tells us its outstanding counts for each proxy ip
	// so we can ensure this is accurate even though a host may die
	// and come back up.
	long m_numOutstandingDownloads;

	// waiting on test url to be downloaded
	bool m_isWaiting;

	// special things used by LoadBucket algo to determine which
	// SpiderProxy to use to download from a particular IP
	long m_countForThisIp;
	long long m_lastTimeUsedForThisIp;

};

// hashtable that maps an ip:port key (64-bits) to a SpiderProxy
static HashTableX s_iptab;

// . handle a udp request for msg 0x54 to get the best proxy to download
//   the url they are requesting to download. this should be ultra fast
//   because we might be downloading 1000+ urls a second, although in that
//   case we should have more hosts that do the proxy load balancing.
// . when a host is done using a proxy to download a url it should 
//   send another 0x54 msg to indicate that.
// . if a host dies then it might not indicate it is done using a proxy
//   so should timeout requests and decrement the proxy load count
//   or if we notice the host is dead we should remove all its load
// . so each outstanding request needs to be kept in a hash table, 
//   which identifies the proxyip/port and the hostid using it and
//   the time it made the request. the key is the proxy ip/port.
class LoadBucket {
public:
	// ip address of the url being downloaded
	long m_urlIp;
	// the time it started
	long long m_downloadStartTimeMS;
	long long m_downloadEndTimeMS;
	// the host using the proxy
	long m_hostId;
	// key is this for m_prTable
	long m_proxyIp;
	long m_proxyPort;
	// id of this loadbucket in case same host is using the same
	// proxy to download the same urlip
	long m_id;
};

// . similar to s_ipTable but maps a URL's ip to a LoadBucket
// . every download request in the last 10 minutes is represented by one
//   LoadBucket
// . that way we can ensure when downloading multiple urls of the same IP
//   that we splay them out evenly over all the proxies
static HashTableX s_loadTable;

// . when the Conf::m_proxyIps parm is updated we call this to rebuild
//   s_iptab, our table of SpiderProxy instances, which has the proxies and 
//   their performance statistics.
// . we try to maintain stats of ip/ports that did NOT change when rebuilding.
bool buildProxyTable ( ) {

	// scan the NEW list of proxy ip/port pairs in g_conf
	char *p = g_conf.m_proxyIps.getBufStart();

	HashTableX tmptab;
	tmptab.set(8,0,16,NULL,0,false,0,"tmptab");

	// scan the user inputted space-separated list of ip:ports
	for ( ; *p ; ) {
		// skip white space
		if ( is_wspace_a(*p) ) continue;
		// scan in an ip:port
		char *s = p; char *portStr = NULL;
		long dc = 0, pc = 0, gc = 0, bc = 0;
		// scan all characters until we hit \0 or another whitespace
		for ( ; *s && !is_wspace_a(*s); s++) {
			if ( *s == '.' ) { dc++; continue; }
			if ( *s == ':' ) { portStr=s; pc++; continue; }
			if ( is_digit(*s) ) { gc++; continue; }
			bc++;
			continue;
		}
		// ensure it is a legit ip:port combo
		char *msg = NULL;
		if ( gc < 4 ) 
			msg = "not enough digits for an ip";
		if ( pc > 1 )
			msg = "too many colons";
		if ( dc != 3 )
			msg = "need 3 dots for an ip address";
		if ( bc )
			msg = "got illegal char in ip:port listing";
		if ( msg ) {
			char c = *s;
			*s = '\0';
			log("buf: %s for %s",msg,p);
			*s = c;
			return false;
		}

		// convert it
		long iplen = s - p;
		if ( portStr ) iplen = portStr - p;
		long ip = atoip(p,iplen);
		// another sanity check
		if ( ip == 0 || ip == -1 ) {
			log("spider: got bad proxy ip for %s",p);
			return false;
		}

		// and the port default is 80
		long port = 80;
		if ( portStr ) port = atol2(portStr+1,s-portStr-1);
		if ( port < 0 || port > 65535 ) {
			log("spider: got bad proxy port for %s",p);
			return false;
		}


		// . we got a legit ip:port
		// . see if already in our table
		unsigned long long ipKey = ip;
		ipKey <<= 16;
		ipKey |= (unsigned short)(port & 0xffff);

		// also store into tmptable to see what we need to remove
		tmptab.addKey(&ipKey);

		// see if in table
		long islot = s_iptab.getSlot( &ipKey);

		// advance p
		p = s;

		// if in there, keep it as is
		if ( islot >= 0 ) continue;

		// otherwise add new entry
		SpiderProxy newThing;
		memset ( &newThing , 0 , sizeof(SpiderProxy));
		newThing.m_ip = ip;
		newThing.m_port = port;
		newThing.m_lastDownloadTookMS = -1;
		newThing.m_lastSuccessfulTestMS = -1;
		if ( ! s_iptab.addKey ( &ipKey, &newThing ) )
			return false;
	}		

 redo:
	// scan all SpiderProxies in tmptab
	for ( long i = 0 ; i < s_iptab.getNumSlots() ; i++ ) {
		// skip empty buckets in hashtable s_iptab
		if ( ! s_iptab.m_flags[i] ) continue;
		// get the key
		long long key = *(long long *)s_iptab.getKey(i);
		// must also exist in tmptab, otherwise it got removed by user
		if ( tmptab.isInTable ( &key ) ) continue;
		// shoot, it got removed. not in the new list of ip:ports
		s_iptab.removeKey ( &key );
		// hashtable is messed up now, start over
		goto redo;
	}

	return true;
}

// save the stats
bool saveSpiderProxyStats ( ) {
	// save hash table
	return s_iptab.save(g_hostdb.m_dir,"spiderproxystats.dat");
}

bool loadSpiderProxyStats ( ) {
	// save hash table
	return s_iptab.load(g_hostdb.m_dir,"spiderproxystats.dat");
}

// . we call this from Parms.cpp which prints out the proxy related controls
//   and this table below them...
// . allows user to see the stats of each spider proxy
bool printSpiderProxyTable ( SafeBuf *sb ) {

	// only host #0 will have the stats ... so print that link
	if ( g_hostdb.m_myHost->m_hostId != 0 ) {
		Host *h = g_hostdb.getHost(0);
		sb->safePrintf("<br>"
			       "<b>See table on <a href=http://%s:%li/"
			       "admin/proxies>"
			       "host #0</a></b>"
			       "<br>"
			       , iptoa(h->m_ip)
			       , (long)(h->m_httpPort)
			       );
		//return true;
	}

	// print host table
	sb->safePrintf ( 
		       "<table %s>"

		       "<tr><td colspan=10><center>"
		       "<b>Spider Proxies "
		       "</b>"
		       "</center></td></tr>" 

		       "<tr bgcolor=#%s>"
		       "<td>"
		       "<b>proxy IP</b></td>"
		       "<td><b>proxy port</b></td>"
		       // time of last successful download. print "none"
		       // if never successfully used
		       "<td><b>test url last successful download</b></td>"
		       // we fetch a test url every minute or so through
		       // each proxy to ensure it is up. typically this should
		       // be your website so you do not make someone angry.
		       "<td><b>test url last download</b></td>"
		       // print "FAILED" in red if it failed to download
		       "<td><b>test url download time</b></td>"

		       "</tr>"
		       
		       , TABLE_STYLE
		       , DARK_BLUE 
			);

	long now = getTimeLocal();

	// print it
	for ( long i = 0 ; i < s_iptab.getNumSlots() ; i++ ) {
		// skip empty slots
		if ( ! s_iptab.m_flags[i] ) continue;

		SpiderProxy *sp = (SpiderProxy *)s_iptab.getValueFromSlot(i);

		char *bg = LIGHT_BLUE;
		// mark with light red bg if last test url attempt failed
		if ( sp->m_lastDownloadTookMS == -1 &&
		     sp->m_lastDownloadTestAttemptMS>0 )
			bg = "ffa6a6";

		// print it
		sb->safePrintf (
			       "<tr bgcolor=#%s>"
			       "<td>%s</td>" // proxy ip
			       "<td>%li</td>" // port
			       , bg
			       , iptoa(sp->m_ip)
			       , (long)sp->m_port
			       );

		// last SUCCESSFUL download time ago. when it completed.
		long ago = now - sp->m_lastSuccessfulTestMS/1000;
		sb->safePrintf("<td>");
		// like 1 minute ago etc.
		if ( sp->m_lastSuccessfulTestMS <= 0 )
			sb->safePrintf("none");
		else
			sb->printTimeAgo ( ago , now );
		sb->safePrintf("</td>");

		// last download time ago
		ago = now - sp->m_lastDownloadTestAttemptMS/1000;
		sb->safePrintf("<td>");
		// like 1 minute ago etc.
		if ( sp->m_lastDownloadTestAttemptMS<= 0 )
			sb->safePrintf("none");
		else
			sb->printTimeAgo ( ago , now );
		sb->safePrintf("</td>");

		// how long to download the test url?
		if ( sp->m_lastDownloadTookMS != -1 )
			sb->safePrintf("<td>%lims</td>",
				       (long)sp->m_lastDownloadTookMS);
		else if ( sp->m_lastDownloadTestAttemptMS<= 0 )
			sb->safePrintf("<td>unknown</td>");
		else
			sb->safePrintf("<td>"
				       "<font color=red>FAILED</font>"
				       "</td>");

		sb->safePrintf("</tr>\n");
	}

	sb->safePrintf("</table><br>");
	return true;
}

// class spip {
// public:
// 	long m_ip;
// 	long m_port;
// };

void gotTestUrlReplyWrapper ( void *state , TcpSocket *s ) {

	//spip *ss = (spip *)state;
	// free that thing
	//mfree ( ss , sizeof(spip) ,"spip" );

	// note it
	log("sproxy: got test url reply: %s",
	    s->m_readBuf);

	// we can get the spider proxy ip/port from the socket because
	// we sent this url download request to that spider proxy
	unsigned long long key = (unsigned long)s->m_ip;
	key <<= 16;
	key |= (unsigned long)s->m_port;

	SpiderProxy *sp = (SpiderProxy *)s_iptab.getValue ( &key );

	// did user remove it from the list before we could finish testing it?
	if ( ! sp ) return;

	sp->m_isWaiting = false;

	// ok, update how long it took to do the download
	long long nowms = gettimeofdayInMillisecondsLocal();
	long long took = nowms - sp->m_lastDownloadTestAttemptMS;
	sp->m_lastDownloadTookMS = (long)took;

	// ETCPTIMEDOUT?
	sp->m_lastDownloadError = g_errno;

	// if we had no error, that was our last successful test
	if ( ! g_errno )
		sp->m_lastSuccessfulTestMS = nowms;

}

// . Process.cpp should call this from its timeout wrapper
// . updates the stats of each proxy
// . returns false and sets g_errno on error
bool downloadTestUrlFromProxies ( ) {

	// only host #0 should do the testing i guess
	//if ( g_hostdb.m_myHost->m_hostId != 0 ) return true;

	// if host #0 dies then host #1 must take its place managing the
	// spider proxies
	Host *h0 = g_hostdb.getFirstAliveHost();
	if ( g_hostdb.m_myHost != h0 ) return true;

	long long nowms = gettimeofdayInMillisecondsLocal();

	for ( long i = 0 ; i < s_iptab.getNumSlots() ; i++ ) {

		// skip empty slots
		if ( ! s_iptab.m_flags[i] ) continue;

		SpiderProxy *sp = (SpiderProxy *)s_iptab.getValueFromSlot(i);

		long long elapsed  = nowms - sp->m_lastDownloadTestAttemptMS;

		// hit test url once per 31 seconds
		if ( elapsed < 31000 ) continue;

		// or if never came back yet!
		if ( sp->m_isWaiting ) continue;

		char *tu = g_conf.m_proxyTestUrl.getBufStart();
		if ( ! tu ) continue;

		//spip *ss = (spip *)mmalloc(8,"sptb");
		//	if ( ! ss ) return false;
		//	ss->m_ip = sp->m_ip;
		//	ss->m_port = sp->m_port;
		

		sp->m_isWaiting = true;

		sp->m_lastDownloadTestAttemptMS = nowms;

		// returns false if blocked
		if ( ! g_httpServer.getDoc( tu ,
					    0 , // ip
					    0 , // offset
					    -1 , // size
					    false , // useifmodsince
					    NULL ,// state
					    gotTestUrlReplyWrapper ,
					    30*1000, // 30 sec timeout
					    sp->m_ip, // proxyip
					    sp->m_port, // proxyport
					    -1, // maxtextdoclen
					    -1 // maxtextotherlen
					    ) ) {
			//blocked++;
			continue;
		}
		// did not block somehow
		sp->m_isWaiting = false;
		// must have been an error then
		sp->m_lastDownloadError = g_errno;
		// free that thing
		//mfree ( ss , sizeof(spip) ,"spip" );
		// log it
		log("sproxy: error downloading test url %s through %s:%li"
		    ,tu,iptoa(sp->m_ip),(long)sp->m_port);
		    
	}
	return true;
}

// a host is asking us (host #0) what proxy to use?
void handleRequest54 ( UdpSlot *udpSlot , long niceness ) {

	char *request     = udpSlot->m_readBuf;
	long  requestSize = udpSlot->m_readBufSize;
	// sanity check
	if ( requestSize != 4 ) {
		log("db: Got bad request 0x54 size of %li bytes. bad",
		    requestSize );
		g_udpServer.sendErrorReply ( udpSlot , EBADREQUESTSIZE );
		return;
	}

	long urlIp = *request;

	// send to a proxy that is up and has the least amount
	// of LoadBuckets with this urlIp, if tied, go to least loaded.

	// clear counts
	for ( long i = 0 ; i < s_iptab.getNumSlots() ; i++ ) {
		// skip empty slots
		if ( ! s_iptab.m_flags[i] ) continue;
		SpiderProxy *sp = (SpiderProxy *)s_iptab.getValueFromSlot(i);
		sp->m_countForThisIp = 0;
		sp->m_lastTimeUsedForThisIp = 0LL;
	}

	// this table maps a url's current IP to a possibly MULTIPLE slots
	// which tell us what proxy is downloading a page from that IP.
	// so we can try to find a proxy that is not download a url from
	// this IP currently, or hasn't been for the longest time...
	long hslot = s_loadTable.getSlot ( &urlIp );
	// scan all proxies that have this urlip outstanding
	for ( long i = hslot ; i >= 0 ; i = s_loadTable.getNextSlot(i,&urlIp)){
		// get the bucket
		LoadBucket **pp;
		pp = (LoadBucket **)s_loadTable.getValueFromSlot(i);
		// get proxy # that has this out
		LoadBucket *lb = *pp;
		// get the spider proxy this load point was for
		unsigned long long key = lb->m_proxyIp;
		key <<= 16;
		key |= (unsigned short)lb->m_proxyPort;
		SpiderProxy *sp = (SpiderProxy *)s_iptab.getValue(&key);
		// must be there unless user remove it from the list
		if ( ! sp ) continue;
		// count it up
		if (  lb->m_downloadEndTimeMS == 0LL ) 
			sp->m_countForThisIp++;
		// set the last time used to the most recently downloaded time
		// that this proxy has downloaded from this ip
		if ( lb->m_downloadEndTimeMS &&
		     lb->m_downloadEndTimeMS > sp->m_lastTimeUsedForThisIp )
			sp->m_lastTimeUsedForThisIp = lb->m_downloadEndTimeMS;
	}

	// first try to get a spider proxy that is not "dead"
	bool skipDead = true;

 redo:
	// get the min of the counts
	long minCount = 999999;
	for ( long i = 0 ; i < s_iptab.getNumSlots() ; i++ ) {
		// skip empty slots
		if ( ! s_iptab.m_flags[i] ) continue;
		// get the spider proxy
		SpiderProxy *sp = (SpiderProxy *)s_iptab.getValueFromSlot(i);
		// if it failed the last test, skip it
		if ( skipDead && sp->m_lastDownloadError ) continue;
		if ( sp->m_countForThisIp >= minCount ) continue;
		minCount = sp->m_countForThisIp;
	}

	// all dead? then get the best dead one
	if ( minCount == 999999 ) {
		skipDead = false;
		goto redo;
	}

	long long oldest = 0x7fffffffffffffff;
	SpiderProxy *winnersp = NULL;
	// now find the best proxy wih the minCount
	for ( long i = 0 ; i < s_iptab.getNumSlots() ; i++ ) {
		// skip empty slots
		if ( ! s_iptab.m_flags[i] ) continue;
		// get the spider proxy
		SpiderProxy *sp = (SpiderProxy *)s_iptab.getValueFromSlot(i);
		// if it failed the last test, skip it... not here...
		if ( skipDead && sp->m_lastDownloadError ) continue;
		// if all hosts were "dead" because they all had 
		// m_lastDownloadError set then minCount will be 999999
		// and nobody should continue from this statement:
		if ( sp->m_countForThisIp > minCount ) continue;
		// then go by last download time for this ip
		if ( sp->m_lastTimeUsedForThisIp >= oldest ) continue;
		// pick the spider proxy used longest ago
		oldest = sp->m_lastTimeUsedForThisIp;
		// got a new winner
		winnersp = sp;
	}

	// we must have a winner
	if ( ! winnersp ) { char *xx=NULL;*xx=0; }

	long long nowms = gettimeofdayInMillisecondsLocal();

	// add a new load bucket then!
	LoadBucket bb;
	bb.m_urlIp = urlIp;
	// the time it started
	bb.m_downloadStartTimeMS = nowms;
	// download has not ended yet
	bb.m_downloadEndTimeMS = 0LL;
	// the host using the proxy
	bb.m_hostId = udpSlot->m_hostId;
	// key is this for m_prTable
	bb.m_proxyIp   = winnersp->m_ip;
	bb.m_proxyPort = winnersp->m_port;
	// a new id. we use this to update the downloadEndTime when done
	static long s_lbid = 0;
	bb.m_id = s_lbid++;
	// add it now
	s_loadTable.addKey ( &urlIp , &bb );

	// and give proxy ip/port back to the requester so they can
	// use that to download their url
	char *p = udpSlot->m_tmpBuf;
	*(long  *)p = winnersp->m_ip  ; p += 4;
	*(short *)p = winnersp->m_port; p += 2;
	// and the loadbucket id
	*(long *)p = bb.m_id; p += 4;

	// now remove old entries from the load table. entries that
	// have completed and have a download end time more than 10 mins ago
	for ( long i = 0 ; i < s_loadTable.getNumSlots() ; i++ ) {
		// skip if empty
		if ( ! s_loadTable.m_flags[i] ) continue;
		// get the bucket
		LoadBucket *pp =(LoadBucket *)s_loadTable.getValueFromSlot(i);
		// skip if still active
		if ( pp->m_downloadEndTimeMS == 0LL ) continue;
		// delta t
		long long took = nowms - pp->m_downloadEndTimeMS;
		// < 10 mins?
		if ( took < 10*60*1000 ) continue;
		// ok, its too old, nuke it
		s_loadTable.removeSlot(i);
		// the keys might have buried us but we really should not
		// mis out on analyzing any keys if we just keep looping here
		// should we? TODO: figure it out. if we miss a few it's not
		// a big deal.
		i--;
	}

	// send the proxy ip/port/LBid back to user
	g_udpServer.sendReply_ass ( udpSlot->m_tmpBuf , // msg
				    10 , // msgSize
				    udpSlot->m_tmpBuf , // alloc
				    10 , 
				    udpSlot , 
				    60 ) ; // 60s timeout
}
	
// use msg 0x55 to say you are done using the proxy
void handleRequest55 ( UdpSlot *udpSlot , long niceness ) {

	char *request     = udpSlot->m_readBuf;
	long  requestSize = udpSlot->m_readBufSize;
	// sanity check
	if ( requestSize != 14 ) {
		log("db: Got bad request 0x55 size of %li bytes. bad",
		    requestSize );
		g_udpServer.sendErrorReply ( udpSlot , EBADREQUESTSIZE );
		return;
	}

	char *p = request;
	long  urlIp     = *(long  *)p; p += 4;
	long  proxyIp   = *(long  *)p; p += 4;
	short proxyPort = *(short *)p; p += 2;
	long  lbId      = *(long  *)p; p += 4;

	//
	// update the load bucket
	//

	// scan over all that match to find lbid
	long hslot = s_loadTable.getSlot ( &urlIp );
	// scan all proxies that have this urlip outstanding
	for ( long i = hslot ; i >= 0 ; i = s_loadTable.getNextSlot(i,&urlIp)){
		// get the bucket
		LoadBucket **pp;
		pp = (LoadBucket **)s_loadTable.getValueFromSlot(i);
		// get proxy # that has this out
		LoadBucket *lb = *pp;
		// is it the right id?
		if ( lbId != lb->m_id ) continue;
		if ( lb->m_proxyIp != proxyIp ) continue;
		if ( lb->m_proxyPort != proxyPort ) continue;
		// that's it. set the download end time
		long long nowms = gettimeofdayInMillisecondsLocal();
		lb->m_downloadEndTimeMS = nowms;
	}

}

// call this at startup to register the handlers
bool initSpiderProxyStuff() {
	
	// do this for all hosts in case host #0 goes dead, then everyone
	// will, according to Msg13.cpp, send to host #1, the next in line
	// if she is alive
	//if ( g_hostdb.m_myHostId != 0 ) return true;

	// only host #0 has handlers
	if ( ! g_udpServer.registerHandler ( 0x54, handleRequest54 )) 
		return false;
	if ( ! g_udpServer.registerHandler ( 0x55, handleRequest55 )) 
		return false;

	// key is ip/port
	s_iptab.set(8,sizeof(SpiderProxy),0,NULL,0,false,0,"siptab");

	loadSpiderProxyStats();

	// build the s_iptab hashtable for the first time
	buildProxyTable ();

	// make the loadtable hashtable
	static bool s_flag = 0;
	if ( s_flag ) return true;
	s_flag = true;
	return s_loadTable.set(4,
			       sizeof(LoadBucket),
			       128,
			       NULL,
			       0,
			       true, // allow dups?
			       MAX_NICENESS,
			       "lbtab");

}

