/* This software was written by Dirk Engling <erdgeist@erdgeist.org>
   It is considered beerware. Prost. Skol. Cheers or whatever.
   Some of the stuff below is stolen from Fefes example libowfat httpd.

   $Id$ */

/* System */
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <pwd.h>
#include <ctype.h>

/* Libowfat */
#include "socket.h"
#include "io.h"
#include "iob.h"
#include "byte.h"
#include "scan.h"
#include "ip6.h"

/* Opentracker */
#include "trackerlogic.h"
#include "ot_mutex.h"
#include "ot_http.h"
#include "ot_udp.h"
#include "ot_accesslist.h"
#include "ot_stats.h"
#include "ot_livesync.h"

/* Globals */
time_t       g_now_seconds;
char *       g_redirecturl;
uint32_t     g_tracker_id;
volatile int g_opentracker_running = 1;
int          g_self_pipe[2];

static char * g_serverdir;

static void panic( const char *routine ) {
  fprintf( stderr, "%s: %s\n", routine, strerror(errno) );
  exit( 111 );
}

static void signal_handler( int s ) {
  if( s == SIGINT ) {
    /* Any new interrupt signal quits the application */
    signal( SIGINT, SIG_DFL);

    /* Tell all other threads to not acquire any new lock on a bucket
       but cancel their operations and return */
    g_opentracker_running = 0;

    trackerlogic_deinit();
    exit( 0 );
  } else if( s == SIGALRM ) {
    /* Maintain our copy of the clock. time() on BSDs is very expensive. */
    g_now_seconds = time(NULL);
    alarm(5);
  }
}

static void usage( char *name ) {
  fprintf( stderr, "Usage: %s [-i ip] [-p port] [-P port] [-r redirect] [-d dir] [-A ip] [-f config] [-s livesyncport]"
#ifdef WANT_ACCESSLIST_BLACK
  " [-b blacklistfile]"
#elif defined ( WANT_ACCESSLIST_WHITE )
  " [-w whitelistfile]"
#endif
  "\n", name );
}

#define HELPLINE(opt,desc) fprintf(stderr, "\t%-10s%s\n",opt,desc)
static void help( char *name ) {
  usage( name );

  HELPLINE("-f config","include and execute the config file");
  HELPLINE("-i ip","specify ip to bind to (default: *, you may specify more than one)");
  HELPLINE("-p port","specify tcp port to bind to (default: 6969, you may specify more than one)");
  HELPLINE("-P port","specify udp port to bind to (default: 6969, you may specify more than one)");
  HELPLINE("-r redirecturl","specify url where / should be redirected to (default none)");
  HELPLINE("-d dir","specify directory to try to chroot to (default: \".\")");
  HELPLINE("-A ip","bless an ip address as admin address (e.g. to allow syncs from this address)");
#ifdef WANT_ACCESSLIST_BLACK
  HELPLINE("-b file","specify blacklist file.");
#elif defined( WANT_ACCESSLIST_WHITE )
  HELPLINE("-w file","specify whitelist file.");
#endif

  fprintf( stderr, "\nExample:   ./opentracker -i 127.0.0.1 -p 6969 -P 6969 -f ./opentracker.conf -i 10.1.1.23 -p 2710 -p 80\n" );
}
#undef HELPLINE

static void handle_dead( const int64 sock ) {
  struct http_data* cookie=io_getcookie( sock );
  if( cookie ) {
    if( cookie->flag & STRUCT_HTTP_FLAG_IOB_USED )
      iob_reset( &cookie->data.batch );
    if( cookie->flag & STRUCT_HTTP_FLAG_ARRAY_USED )
      array_reset( &cookie->data.request );
    if( cookie->flag & STRUCT_HTTP_FLAG_WAITINGFORTASK )
      mutex_workqueue_canceltask( sock );
    free( cookie );
  }
  io_close( sock );
}

static ssize_t handle_read( const int64 sock, struct ot_workstruct *ws ) {
  struct http_data* cookie = io_getcookie( sock );
  ssize_t byte_count;

  if( ( byte_count = io_tryread( sock, ws->inbuf, G_INBUF_SIZE ) ) <= 0 ) {
    handle_dead( sock );
    return 0;
  }

  /* If we get the whole request in one packet, handle it without copying */
  if( !array_start( &cookie->data.request ) ) {
    if( memchr( ws->inbuf, '\n', byte_count ) ) {
      ws->request = ws->inbuf;
      ws->request_size = byte_count;
      return http_handle_request( sock, ws );
    }

    /* ... else take a copy */
    cookie->flag |= STRUCT_HTTP_FLAG_ARRAY_USED;
    array_catb( &cookie->data.request, ws->inbuf, byte_count );
    return 0;
  }

  array_catb( &cookie->data.request, ws->inbuf, byte_count );

  if( array_failed( &cookie->data.request ) ||
      array_bytes( &cookie->data.request ) > 8192 )
    return http_issue_error( sock, ws, CODE_HTTPERROR_500 );

  if( !memchr( array_start( &cookie->data.request ), '\n', array_bytes( &cookie->data.request ) ) )
    return 0;

  ws->request      = array_start( &cookie->data.request );
  ws->request_size = array_bytes( &cookie->data.request );
  return http_handle_request( sock, ws );
}

static void handle_write( const int64 sock ) {
  struct http_data* cookie=io_getcookie( sock );
  if( !cookie || ( iob_send( sock, &cookie->data.batch ) <= 0 ) )
    handle_dead( sock );
}

static void handle_accept( const int64 serversocket ) {
  struct http_data *cookie;
  int64 sock;
  ot_ip6 ip;
  uint16 port;
  tai6464 t;

  while( ( sock = socket_accept6( serversocket, ip, &port, NULL ) ) != -1 ) {

    /* Put fd into a non-blocking mode */
    io_nonblock( sock );

    if( !io_fd( sock ) ||
        !( cookie = (struct http_data*)malloc( sizeof(struct http_data) ) ) ) {
      io_close( sock );
      continue;
    }
    io_setcookie( sock, cookie );
    io_wantread( sock );

    memset(cookie, 0, sizeof( struct http_data ) );
    memcpy(cookie->ip,ip,sizeof(ot_ip6));

    stats_issue_event( EVENT_ACCEPT, FLAG_TCP, (uintptr_t)ip);

    /* That breaks taia encapsulation. But there is no way to take system
       time this often in FreeBSD and libowfat does not allow to set unix time */
    taia_uint( &t, 0 ); /* Clear t */
    tai_unix( &(t.sec), (g_now_seconds + OT_CLIENT_TIMEOUT) );
    io_timeout( sock, t );
  }

  if( errno == EAGAIN )
    io_eagain( serversocket );
}

static void server_mainloop( ) {
  struct ot_workstruct ws;
  time_t next_timeout_check = g_now_seconds + OT_CLIENT_TIMEOUT_CHECKINTERVAL;
  struct iovec *iovector;
  int    iovec_entries;

  /* Initialize our "thread local storage" */
  ws.inbuf   = malloc( G_INBUF_SIZE );
  ws.outbuf  = malloc( G_OUTBUF_SIZE );
#ifdef _DEBUG_HTTPERROR
  ws.debugbuf= malloc( G_DEBUGBUF_SIZE );
#endif
  if( !ws.inbuf || !ws.outbuf )
    panic( "Initializing worker failed" );

  for( ; ; ) {
    int64 sock;

    io_wait();

    while( ( sock = io_canread( ) ) != -1 ) {
      const void *cookie = io_getcookie( sock );
      if( (intptr_t)cookie == FLAG_TCP )
        handle_accept( sock );
      else if( (intptr_t)cookie == FLAG_UDP )
        handle_udp6( sock, &ws );
      else if( (intptr_t)cookie == FLAG_SELFPIPE )
        io_tryread( sock, ws.inbuf, G_INBUF_SIZE );
      else
        handle_read( sock, &ws );
    }

    while( ( sock = mutex_workqueue_popresult( &iovec_entries, &iovector ) ) != -1 )
      http_sendiovecdata( sock, &ws, iovec_entries, iovector );

    while( ( sock = io_canwrite( ) ) != -1 )
      handle_write( sock );

    if( g_now_seconds > next_timeout_check ) {
      while( ( sock = io_timeouted() ) != -1 )
        handle_dead( sock );
      next_timeout_check = g_now_seconds + OT_CLIENT_TIMEOUT_CHECKINTERVAL;
    }

    livesync_ticker();

    /* Enforce setting the clock */
    signal_handler( SIGALRM );
  }
}

static int64_t ot_try_bind( ot_ip6 ip, uint16_t port, PROTO_FLAG proto ) {
  int64 sock = proto == FLAG_TCP ? socket_tcp6( ) : socket_udp6( );

#ifndef WANT_V6
  if( !ip6_isv4mapped(ip) ) {
    exerr( "V4 Tracker is V4 only!" );
  }
#else
  if( ip6_isv4mapped(ip) ) {
    exerr( "V6 Tracker is V6 only!" );
  }
#endif

#ifdef _DEBUG
  char *protos[] = {"TCP","UDP","UDP mcast"};
  char _debug[512];
  int off = snprintf( _debug, sizeof(_debug), "Binding socket type %s to address [", protos[proto] );
  off += fmt_ip6c( _debug+off, ip);
  snprintf( _debug + off, sizeof(_debug)-off, "]:%d...", port);
  fputs( _debug, stderr );
#endif

  if( socket_bind6_reuse( sock, ip, port, 0 ) == -1 )
    panic( "socket_bind6_reuse" );

  if( ( proto == FLAG_TCP ) && ( socket_listen( sock, SOMAXCONN) == -1 ) )
    panic( "socket_listen" );

  if( !io_fd( sock ) )
    panic( "io_fd" );

  io_setcookie( sock, (void*)proto );

  io_wantread( sock );

#ifdef _DEBUG
  fputs( " success.\n", stderr);
#endif

  return sock;
}

char * set_config_option( char **option, char *value ) {
#ifdef _DEBUG
  fprintf( stderr, "Setting config option: %s\n", value );
#endif
  while( isspace(*value) ) ++value;
  free( *option );
  return *option = strdup( value );
}

static int scan_ip6_port( const char *src, ot_ip6 ip, uint16 *port ) {
  const char *s = src;
  int off, bracket = 0;
  while( isspace(*s) ) ++s;
  if( *s == '[' ) ++s, ++bracket; /* for v6 style notation */
  if( !(off = scan_ip6( s, ip ) ) )
    return 0;
  s += off;
  if( *s == 0 || isspace(*s)) return s-src;
  if( *s == ']' && bracket ) ++s;
  if( !ip6_isv4mapped(ip)){
    if( ( bracket && *(s) != ':' ) || ( *(s) != '.' ) ) return 0;
    s++;
  } else {
    if( *(s++) != ':' ) return 0;
  }
  if( !(off = scan_ushort (s, port ) ) )
     return 0;
  return off+s-src;
}

int parse_configfile( char * config_filename ) {
  FILE *  accesslist_filehandle;
  char    inbuf[512];
  ot_ip6  tmpip;
  int     bound = 0;

  accesslist_filehandle = fopen( config_filename, "r" );

  if( accesslist_filehandle == NULL ) {
    fprintf( stderr, "Warning: Can't open config file: %s.", config_filename );
    return 0;
  }

  while( fgets( inbuf, sizeof(inbuf), accesslist_filehandle ) ) {
    char *newl;
    char *p = inbuf;

    /* Skip white spaces */
    while(isspace(*p)) ++p;

    /* Ignore comments and empty lines */
    if((*p=='#')||(*p=='\n')||(*p==0)) continue;

    /* chomp */
    if(( newl = strchr(p, '\n' ))) *newl = 0;

    /* Scan for commands */
    if(!byte_diff(p,15,"tracker.rootdir" ) && isspace(p[15])) {
      set_config_option( &g_serverdir, p+16 );
    } else if(!byte_diff(p,14,"listen.tcp_udp" ) && isspace(p[14])) {
      uint16_t tmpport = 6969;
      if( !scan_ip6_port( p+15, tmpip, &tmpport )) goto parse_error;
      ot_try_bind( tmpip, tmpport, FLAG_TCP ); ++bound;
      ot_try_bind( tmpip, tmpport, FLAG_UDP ); ++bound;
    } else if(!byte_diff(p,10,"listen.tcp" ) && isspace(p[10])) {
      uint16_t tmpport = 6969;
      if( !scan_ip6_port( p+11, tmpip, &tmpport )) goto parse_error;
      ot_try_bind( tmpip, tmpport, FLAG_TCP );
      ++bound;
    } else if(!byte_diff(p, 10, "listen.udp" ) && isspace(p[10])) {
      uint16_t tmpport = 6969;
      if( !scan_ip6_port( p+11, tmpip, &tmpport )) goto parse_error;
      ot_try_bind( tmpip, tmpport, FLAG_UDP );
      ++bound;
#ifdef WANT_ACCESSLIST_WHITE
    } else if(!byte_diff(p, 16, "access.whitelist" ) && isspace(p[16])) {
      set_config_option( &g_accesslist_filename, p+17 );
#elif defined( WANT_ACCESSLIST_BLACK )
    } else if(!byte_diff(p, 16, "access.blacklist" ) && isspace(p[16])) {
      set_config_option( &g_accesslist_filename, p+17 );
#endif
#ifdef WANT_RESTRICT_STATS
    } else if(!byte_diff(p, 12, "access.stats" ) && isspace(p[12])) {
      if( !scan_ip6( p+13, tmpip )) goto parse_error;
      accesslist_blessip( tmpip, OT_PERMISSION_MAY_STAT );
#endif
    } else if(!byte_diff(p, 17, "access.stats_path" ) && isspace(p[17])) {
      set_config_option( &g_stats_path, p+18 );
#ifdef WANT_IP_FROM_PROXY
    } else if(!byte_diff(p, 12, "access.proxy" ) && isspace(p[12])) {
      if( !scan_ip6( p+13, tmpip )) goto parse_error;
      accesslist_blessip( tmpip, OT_PERMISSION_MAY_PROXY );
#endif
    } else if(!byte_diff(p, 20, "tracker.redirect_url" ) && isspace(p[20])) {
      set_config_option( &g_redirecturl, p+21 );
#ifdef WANT_SYNC_LIVE
    } else if(!byte_diff(p, 24, "livesync.cluster.node_ip" ) && isspace(p[24])) {
      if( !scan_ip6( p+25, tmpip )) goto parse_error;
      accesslist_blessip( tmpip, OT_PERMISSION_MAY_LIVESYNC );
    } else if(!byte_diff(p, 23, "livesync.cluster.listen" ) && isspace(p[23])) {
      uint16_t tmpport = LIVESYNC_PORT;
      if( !scan_ip6_port( p+24, tmpip, &tmpport )) goto parse_error;
      livesync_bind_mcast( tmpip, tmpport );
#endif
    } else
      fprintf( stderr, "Unhandled line in config file: %s\n", inbuf );
    continue;
  parse_error:
      fprintf( stderr, "Parse error in config file: %s\n", inbuf);
  }
  fclose( accesslist_filehandle );
  return bound;
}

void load_state(const char * const state_filename ) {
  FILE *  state_filehandle;
  char    inbuf[512];
  ot_hash infohash;
  unsigned long long base, downcount;
  int consumed;

  state_filehandle = fopen( state_filename, "r" );

  if( state_filehandle == NULL ) {
    fprintf( stderr, "Warning: Can't open config file: %s.", state_filename );
    return;
  }

  /* We do ignore anything that is not of the form "^[:xdigit:]:\d+:\d+" */
  while( fgets( inbuf, sizeof(inbuf), state_filehandle ) ) {
    int i;
    for( i=0; i<(int)sizeof(ot_hash); ++i ) {
      int eger = 16 * scan_fromhex( inbuf[ 2*i ] ) + scan_fromhex( inbuf[ 1 + 2*i ] );
      if( eger < 0 )
        continue;
      infohash[i] = eger;
    }

    if( i != (int)sizeof(ot_hash) ) continue;
    i *= 2;

    if( inbuf[ i++ ] != ':' || !( consumed = scan_ulonglong( inbuf+i, &base ) ) ) continue;
    i += consumed;
    if( inbuf[ i++ ] != ':' || !( consumed = scan_ulonglong( inbuf+i, &downcount ) ) ) continue;
    add_torrent_from_saved_state( infohash, base, downcount );
  }
    
  fclose( state_filehandle );
}

int drop_privileges (const char * const serverdir) {
  struct passwd *pws = NULL;

  /* Grab pws entry before chrooting */
  pws = getpwnam( "nobody" );
  endpwent();

  if( geteuid() == 0 ) {
    /* Running as root: chroot and drop privileges */
    if(chroot( serverdir )) {
      fprintf( stderr, "Could not chroot to %s, because: %s\n", serverdir, strerror(errno) );
      return -1;
    }

    if(chdir("/"))
      panic("chdir() failed after chrooting: ");

    if( !pws ) {
      setegid( (gid_t)-2 ); setgid( (gid_t)-2 );
      setuid( (uid_t)-2 );  seteuid( (uid_t)-2 );
    }
    else {
      setegid( pws->pw_gid ); setgid( pws->pw_gid );
      setuid( pws->pw_uid );  seteuid( pws->pw_uid );
    }

    if( geteuid() == 0 || getegid() == 0 )
      panic("Still running with root privileges?!");
  }
  else {
    /* Normal user, just chdir() */
    if(chdir( serverdir )) {
      fprintf( stderr, "Could not chroot to %s, because: %s\n", serverdir, strerror(errno) );
      return -1;
    }
  }

  return 0;
}

int main( int argc, char **argv ) {
  ot_ip6 serverip, tmpip;
  int bound = 0, scanon = 1;
  uint16_t tmpport;

  memset( serverip, 0, sizeof(ot_ip6) );
#ifndef WANT_V6
  serverip[10]=serverip[11]=0xff;
  noipv6=1;
#endif

while( scanon ) {
    switch( getopt( argc, argv, ":i:p:A:P:d:r:s:f:l:v"
#ifdef WANT_ACCESSLIST_BLACK
"b:"
#elif defined( WANT_ACCESSLIST_WHITE )
"w:"
#endif
    "h" ) ) {
      case -1 : scanon = 0; break;
      case 'i':
        if( !scan_ip6( optarg, serverip )) { usage( argv[0] ); exit( 1 ); }
        break;
#ifdef WANT_ACCESSLIST_BLACK
      case 'b': set_config_option( &g_accesslist_filename, optarg); break;
#elif defined( WANT_ACCESSLIST_WHITE )
      case 'w': set_config_option( &g_accesslist_filename, optarg); break;
#endif
      case 'p':
        if( !scan_ushort( optarg, &tmpport)) { usage( argv[0] ); exit( 1 ); }
        ot_try_bind( serverip, tmpport, FLAG_TCP ); bound++; break;
      case 'P':
        if( !scan_ushort( optarg, &tmpport)) { usage( argv[0] ); exit( 1 ); }
        ot_try_bind( serverip, tmpport, FLAG_UDP ); bound++; break;
#ifdef WANT_SYNC_LIVE
      case 's':
        if( !scan_ushort( optarg, &tmpport)) { usage( argv[0] ); exit( 1 ); }
        livesync_bind_mcast( serverip, tmpport); break;
#endif
      case 'd': set_config_option( &g_serverdir, optarg ); break;
      case 'r': set_config_option( &g_redirecturl, optarg ); break;
      case 'l': load_state( optarg ); break;
      case 'A':
        if( !scan_ip6( optarg, tmpip )) { usage( argv[0] ); exit( 1 ); }
        accesslist_blessip( tmpip, 0xffff ); /* Allow everything for now */
        break;
      case 'f': bound += parse_configfile( optarg ); break;
      case 'h': help( argv[0] ); exit( 0 );
      case 'v': {
        char buffer[8192];
        stats_return_tracker_version( buffer );
        fputs( buffer, stderr );
        exit( 0 );
      }
      default:
      case '?': usage( argv[0] ); exit( 1 );
    }
  }

  /* Bind to our default tcp/udp ports */
  if( !bound) {
    ot_try_bind( serverip, 6969, FLAG_TCP );
    ot_try_bind( serverip, 6969, FLAG_UDP );
  }

  if( drop_privileges( g_serverdir ? g_serverdir : "." ) == -1 )
    panic( "drop_privileges failed, exiting. Last error");

  signal( SIGPIPE, SIG_IGN );
  signal( SIGINT,  signal_handler );
  signal( SIGALRM, signal_handler );

  g_now_seconds = time( NULL );

  /* Create our self pipe which allows us to interrupt mainloops
     io_wait in case some data is available to send out */
  if( pipe( g_self_pipe ) == -1 )
    panic( "selfpipe failed: " );
  if( !io_fd( g_self_pipe[0] ) )
    panic( "selfpipe io_fd failed: " );
  io_setcookie( g_self_pipe[0], (void*)FLAG_SELFPIPE );
  io_wantread( g_self_pipe[0] );

  /* Init all sub systems. This call may fail with an exit() */
  trackerlogic_init( );

  /* Kick off our initial clock setting alarm */
  alarm(5);

  server_mainloop( );

  return 0;
}

const char *g_version_opentracker_c = "$Source$: $Revision$\n";
