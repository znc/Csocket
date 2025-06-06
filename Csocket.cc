/**
 * @file Csocket.cc
 * @author Jim Hull <csocket@jimloco.com>
 *
 *    Copyright (c) 1999-2012 Jim Hull <csocket@jimloco.com>
 *    All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list
 * of conditions and the following disclaimer in the documentation and/or other materials
 * provided with the distribution.
 * Redistributions in any form must be accompanied by information on how to obtain
 * complete source code for this software and any accompanying software that uses this software.
 * The source code must either be included in the distribution or be available for no more than
 * the cost of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions. For an executable file, complete source code means the source
 * code for all modules it contains. It does not include source code for modules or files
 * that typically accompany the major components of the operating system on which the executable file runs.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OF THIS SOFTWARE BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/***
 * doing this because there seems to be a bug that is losing the "short" on htons when in optimize mode turns into a macro
 * gcc 4.3.4
 */
#if defined(__OPTIMIZE__) && __GNUC__ == 4 && __GNUC_MINOR__ >= 3
#pragma GCC diagnostic warning "-Wconversion"
#endif /* defined(__OPTIMIZE__) && __GNUC__ == 4 && __GNUC_MINOR__ >= 3 */

#include "Csocket.h"
#ifdef __NetBSD__
#include <sys/param.h>
#endif /* __NetBSD__ */

#ifdef HAVE_LIBSSL
#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/dsa.h>
#include <openssl/rsa.h>
#ifndef OPENSSL_NO_COMP
#include <openssl/comp.h>
#endif
#ifndef OPENSSL_NO_ENGINE
#include <openssl/engine.h>
#endif
#define HAVE_ERR_REMOVE_STATE
#ifdef OPENSSL_VERSION_NUMBER
# if OPENSSL_VERSION_NUMBER >= 0x10000000
#  undef HAVE_ERR_REMOVE_STATE
#  define HAVE_ERR_REMOVE_THREAD_STATE
# endif
# if OPENSSL_VERSION_NUMBER < 0x10001000
#  define OPENSSL_NO_TLS1_1            /* 1.0.1-pre~: openssl/openssl@637f374ad49d5f6d4f81d87d7cdd226428aa470c */
#  define OPENSSL_NO_TLS1_2            /* 1.0.1-pre~: openssl/openssl@7409d7ad517650db332ae528915a570e4e0ab88b */
# endif
# if !defined(LIBRESSL_VERSION_NUMBER) || (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER >= 0x03050000fL)
#  if OPENSSL_VERSION_NUMBER >= 0x10100000
#   undef HAVE_ERR_REMOVE_THREAD_STATE /* 1.1.0-pre4: openssl/openssl@8509dcc9f319190c565ab6baad7c88d37a951d1c */
#   undef OPENSSL_NO_SSL2              /* 1.1.0-pre4: openssl/openssl@e80381e1a3309f5d4a783bcaa508a90187a48882 */
#   define OPENSSL_NO_SSL2             /* 1.1.0-pre1: openssl/openssl@45f55f6a5bdcec411ef08a6f8aae41d5d3d234ad */
#   define HAVE_FLEXIBLE_TLS_METHOD    /* 1.1.0-pre1: openssl/openssl@32ec41539b5b23bc42503589fcc5be65d648d1f5 */
#   define HAVE_OPAQUE_SSL
#  endif
# endif /* LIBRESSL_VERSION_NUMBER */
#endif /* OPENSSL_VERSION_NUMBER */
#endif /* HAVE_LIBSSL */

#ifdef HAVE_ICU
#include <unicode/ustring.h>
#include <unicode/errorcode.h>
#include <unicode/ucnv_cb.h>
#endif /* HAVE_ICU */

#include <list>
#include <algorithm>
#include <chrono>

#define CS_SRANDBUFFER 128

/*
 * timeradd/timersub is missing on solaris' sys/time.h, provide
 * some fallback macros
 */
#ifndef	timeradd
#define timeradd(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
		if ((result)->tv_usec >= 1000000) { \
			++(result)->tv_sec; \
			(result)->tv_usec -= 1000000; \
		} \
	} while (0)
#endif

#ifndef timersub
#define timersub(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
		if ((result)->tv_usec < 0) { \
			--(result)->tv_sec; \
			(result)->tv_usec += 1000000; \
		} \
	} while (0)
#endif

using std::stringstream;
using std::ostream;
using std::endl;
using std::min;
using std::vector;

#define CREATE_ARES_VER( a, b, c ) ((a<<16)|(b<<8)|c)

#ifndef _NO_CSOCKET_NS // some people may not want to use a namespace
namespace Csocket
{
#endif /* _NO_CSOCKET_NS */

static int s_iCsockSSLIdx = 0; //!< this gets setup once in InitSSL
int GetCsockSSLIdx()
{
	return( s_iCsockSSLIdx );
}

#ifdef _WIN32

#if defined(_WIN32) && (!defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600))
//! thanks to KiNgMaR @ #znc for this wrapper
static int inet_pton( int af, const char *src, void *dst )
{
	sockaddr_storage aAddress;
	int iAddrLen = sizeof( sockaddr_storage );
	memset( &aAddress, 0, iAddrLen );
	char * pTmp = strdup( src );
	aAddress.ss_family = af; // this is important:
	// The function fails if the sin_family member of the SOCKADDR_IN structure is not set to AF_INET or AF_INET6.
	int iRet = WSAStringToAddressA( pTmp, af, NULL, ( sockaddr * )&aAddress, &iAddrLen );
	free( pTmp );
	if( iRet == 0 )
	{
		if( af == AF_INET6 )
			memcpy( dst, &( ( sockaddr_in6 * ) &aAddress )->sin6_addr, sizeof( in6_addr ) );
		else
			memcpy( dst, &( ( sockaddr_in * ) &aAddress )->sin_addr, sizeof( in_addr ) );
		return( 1 );
	}
	return( -1 );
}
#endif

static inline void set_non_blocking( cs_sock_t fd )
{
	u_long iOpts = 1;
	ioctlsocket( fd, FIONBIO, &iOpts );
}

/*
 * not used by anything anymore
static inline void set_blocking(cs_sock_t fd)
{
	u_long iOpts = 0;
	ioctlsocket( fd, FIONBIO, &iOpts );
}
*/

static inline void set_close_on_exec( cs_sock_t fd )
{
	// TODO add this for windows
	// see http://gcc.gnu.org/ml/java-patches/2002-q1/msg00696.html
	// for infos on how to do this
}

#else	// _WIN32

static inline void set_non_blocking( cs_sock_t fd )
{
	int fdflags = fcntl( fd, F_GETFL, 0 );
	if( fdflags < 0 )
		return; // Ignore errors
	fcntl( fd, F_SETFL, fdflags|O_NONBLOCK );
}

/*
 * not used by anything anymore
static inline void set_blocking(cs_sock_t fd)
{
	int fdflags = fcntl(fd, F_GETFL, 0);
	if( fdflags < 0 )
		return; // Ignore errors
	fdflags &= ~O_NONBLOCK;
	fcntl( fd, F_SETFL, fdflags );
}
*/

static inline void set_close_on_exec( cs_sock_t fd )
{
	int fdflags = fcntl( fd, F_GETFD, 0 );
	if( fdflags < 0 )
		return; // Ignore errors
	fcntl( fd, F_SETFD, fdflags|FD_CLOEXEC );
}
#endif /* _WIN32 */

void CSSockAddr::SinFamily()
{
#ifdef HAVE_IPV6
	m_saddr6.sin6_family = PF_INET6;
#endif /* HAVE_IPV6 */
	m_saddr.sin_family = PF_INET;
}

void CSSockAddr::SinPort( uint16_t iPort )
{
#ifdef HAVE_IPV6
	m_saddr6.sin6_port = htons( iPort );
#endif /* HAVE_IPV6 */
	m_saddr.sin_port = htons( iPort );
}

void CSSockAddr::SetIPv6( bool b )
{
#ifndef HAVE_IPV6
	if( b )
	{
		CS_DEBUG( "-DHAVE_IPV6 must be set during compile time to enable this feature" );
		m_bIsIPv6 = false;
		return;
	}
#endif /* HAVE_IPV6 */
	m_bIsIPv6 = b;
	SinFamily();
}


#ifdef HAVE_LIBSSL
static int _PemPassCB( char *pBuff, int iBuffLen, int rwflag, void * pcSocket )
{
	Csock * pSock = static_cast<Csock *>( pcSocket );
	const CS_STRING & sPassword = pSock->GetPemPass();
	if( iBuffLen <= 0 )
		return( 0 );
	memset( pBuff, '\0', iBuffLen );
	if( sPassword.empty() )
		return( 0 );
	int iUseBytes = min( iBuffLen - 1, ( int )sPassword.length() );
	memcpy( pBuff, sPassword.data(), iUseBytes );
	return( iUseBytes );
}

static int _CertVerifyCB( int preverify_ok, X509_STORE_CTX *x509_ctx )
{
	Csock * pSock = GetCsockFromCTX( x509_ctx );
	if( pSock )
		return( pSock->VerifyPeerCertificate( preverify_ok, x509_ctx ) );

   return( preverify_ok );
}

static void _InfoCallback( const SSL * pSSL, int where, int ret )
{
	if( ( where & SSL_CB_HANDSHAKE_DONE ) && ret != 0 )
	{
		Csock * pSock = static_cast<Csock *>( SSL_get_ex_data( pSSL, GetCsockSSLIdx() ) );
		if( pSock )
			pSock->SSLHandShakeFinished();
	}
}


Csock * GetCsockFromCTX( X509_STORE_CTX * pCTX )
{
	Csock * pSock = NULL;
	SSL * pSSL = ( SSL * ) X509_STORE_CTX_get_ex_data( pCTX, SSL_get_ex_data_X509_STORE_CTX_idx() );
	if( pSSL )
		pSock = ( Csock * ) SSL_get_ex_data( pSSL, GetCsockSSLIdx() );
	return( pSock );
}
#endif /* HAVE_LIBSSL */


#ifdef USE_GETHOSTBYNAME

// this issue here is getaddrinfo has a significant behavior difference when dealing with round robin dns on an
// ipv4 network. This is not desirable IMHO. so when this is compiled without ipv6 support backwards compatibility
// is maintained.
static int __GetHostByName( const CS_STRING & sHostName, struct in_addr * paddr, u_int iNumRetries )
{
	int iReturn = HOST_NOT_FOUND;
	struct hostent * hent = NULL;
#ifdef __linux__
	char hbuff[2048];
	struct hostent hentbuff;

	int err;
	for( u_int a = 0; a < iNumRetries; ++a )
	{
		memset( ( char * ) hbuff, '\0', 2048 );
		iReturn = gethostbyname_r( sHostName.c_str(), &hentbuff, hbuff, 2048, &hent, &err );

		if( iReturn == 0 )
			break;

		if( iReturn != TRY_AGAIN )
		{
			CS_DEBUG( "gethostyname_r: " << hstrerror( h_errno ) );
			break;
		}
	}
	if( !hent && iReturn == 0 )
		iReturn = HOST_NOT_FOUND;
#else
	for( u_int a = 0; a < iNumRetries; ++a )
	{
		iReturn = HOST_NOT_FOUND;
		hent = gethostbyname( sHostName.c_str() );

		if( hent )
		{
			iReturn = 0;
			break;
		}

		if( h_errno != TRY_AGAIN )
		{
#ifndef _WIN32
			CS_DEBUG( "gethostyname: " << hstrerror( h_errno ) );
#endif /* _WIN32 */
			break;
		}
	}

#endif /* __linux__ */

	if( iReturn == 0 )
		memcpy( &paddr->s_addr, hent->h_addr_list[0], sizeof( paddr->s_addr ) );

	return( iReturn == TRY_AGAIN ? EAGAIN : iReturn );
}
#endif /* !USE_GETHOSTBYNAME */


#ifdef HAVE_C_ARES
void Csock::FreeAres()
{
	if( m_pARESChannel )
	{
		ares_destroy( m_pARESChannel );
		m_pARESChannel = NULL;
	}
}

static void AresHostCallback( void * pArg, int status, int timeouts, struct hostent *hent )
{
	Csock * pSock = ( Csock * )pArg;
	if( status == ARES_SUCCESS && hent && hent->h_addr_list[0] != NULL )
	{
		CSSockAddr * pSockAddr = pSock->GetCurrentAddr();
		if( hent->h_addrtype == AF_INET )
		{
			pSock->SetIPv6( false );
			memcpy( pSockAddr->GetAddr(), hent->h_addr_list[0], sizeof( *( pSockAddr->GetAddr() ) ) );
		}
#ifdef HAVE_IPV6
		else if( hent->h_addrtype == AF_INET6 )
		{
			pSock->SetIPv6( true );
			memcpy( pSockAddr->GetAddr6(), hent->h_addr_list[0], sizeof( *( pSockAddr->GetAddr6() ) ) );
		}
#endif /* HAVE_IPV6 */
		else
		{
			status = ARES_ENOTFOUND;
		}
	}
	else
	{
		CS_DEBUG( ares_strerror( status ) );
		if( status == ARES_SUCCESS )
		{
			CS_DEBUG( "Received ARES_SUCCESS without any useful reply, using NODATA instead" );
			status = ARES_ENODATA;
		}
	}
	pSock->SetAresFinished( status );
}
#endif /* HAVE_C_ARES */

CGetAddrInfo::CGetAddrInfo( const CS_STRING & sHostname, Csock * pSock, CSSockAddr & csSockAddr )
	: m_pSock( pSock ), m_csSockAddr( csSockAddr )
{
	m_sHostname = sHostname;
	m_pAddrRes = NULL;
	m_iRet = ETIMEDOUT;
}

CGetAddrInfo::~CGetAddrInfo()
{
	if( m_pAddrRes )
		freeaddrinfo( m_pAddrRes );
	m_pAddrRes = NULL;
}

void CGetAddrInfo::Init()
{
	memset( ( struct addrinfo * )&m_cHints, '\0', sizeof( m_cHints ) );
	m_cHints.ai_family = m_csSockAddr.GetAFRequire();

	m_cHints.ai_socktype = SOCK_STREAM;
	m_cHints.ai_protocol = IPPROTO_TCP;
#ifdef AI_ADDRCONFIG
	// this is suppose to eliminate host from appearing that this system can not support
	m_cHints.ai_flags = AI_ADDRCONFIG;
#endif /* AI_ADDRCONFIG */

	if( m_pSock && ( m_pSock->GetType() == Csock::LISTENER || m_pSock->GetConState() == Csock::CST_BINDVHOST ) )
	{
		// when doing a dns for bind only, set the AI_PASSIVE flag as suggested by the man page
		m_cHints.ai_flags |= AI_PASSIVE;
	}
}

int CGetAddrInfo::Process()
{
	m_iRet = getaddrinfo( m_sHostname.c_str(), NULL, &m_cHints, &m_pAddrRes );
	if( m_iRet == EAI_AGAIN )
		return( EAGAIN );
	else if( m_iRet == 0 )
		return( 0 );
	return( ETIMEDOUT );
}

int CGetAddrInfo::Finish()
{
	if( m_iRet == 0 && m_pAddrRes )
	{
		std::list<struct addrinfo *> lpTryAddrs;
		bool bFound = false;
		for( struct addrinfo * pRes = m_pAddrRes; pRes; pRes = pRes->ai_next )
		{
			// pass through the list building out a lean list of candidates to try. AI_CONFIGADDR doesn't always seem to work
#ifdef __sun
			if( ( pRes->ai_socktype != SOCK_STREAM ) || ( pRes->ai_protocol != IPPROTO_TCP && pRes->ai_protocol != IPPROTO_IP ) )
#else
			if( ( pRes->ai_socktype != SOCK_STREAM ) || ( pRes->ai_protocol != IPPROTO_TCP ) )
#endif /* __sun work around broken impl of getaddrinfo */
				continue;

			if( ( m_csSockAddr.GetAFRequire() != CSSockAddr::RAF_ANY ) && ( pRes->ai_family != m_csSockAddr.GetAFRequire() ) )
				continue; // they requested a special type, so be certain we woop past anything unwanted
			lpTryAddrs.push_back( pRes );
		}
		for( std::list<struct addrinfo *>::iterator it = lpTryAddrs.begin(); it != lpTryAddrs.end(); )
		{
			// cycle through these, leaving the last iterator for the outside caller to call, so if there is an error it can call the events
			struct addrinfo * pRes = *it;
			bool bTryConnect = false;
			if( pRes->ai_family == AF_INET )
			{
				if( m_pSock )
					m_pSock->SetIPv6( false );
				m_csSockAddr.SetIPv6( false );
				struct sockaddr_in * pTmp = ( struct sockaddr_in * )pRes->ai_addr;
				memcpy( m_csSockAddr.GetAddr(), &( pTmp->sin_addr ), sizeof( *( m_csSockAddr.GetAddr() ) ) );
				if( m_pSock && m_pSock->GetConState() == Csock::CST_DESTDNS && m_pSock->GetType() == Csock::OUTBOUND )
				{
					bTryConnect = true;
				}
				else
				{
					bFound = true;
					break;
				}
			}
#ifdef HAVE_IPV6
			else if( pRes->ai_family == AF_INET6 )
			{
				if( m_pSock )
					m_pSock->SetIPv6( true );
				m_csSockAddr.SetIPv6( true );
				struct sockaddr_in6 * pTmp = ( struct sockaddr_in6 * )pRes->ai_addr;
				memcpy( m_csSockAddr.GetAddr6(), &( pTmp->sin6_addr ), sizeof( *( m_csSockAddr.GetAddr6() ) ) );
				if( m_pSock && m_pSock->GetConState() == Csock::CST_DESTDNS && m_pSock->GetType() == Csock::OUTBOUND )
				{
					bTryConnect = true;
				}
				else
				{
					bFound = true;
					break;
				}
			}
#endif /* HAVE_IPV6 */

			++it; // increment the iterator her so we know if its the last element or not

			if( bTryConnect && it != lpTryAddrs.end() )
			{
				// save the last attempt for the outer loop, the issue then becomes that the error is thrown on the last failure
				if( m_pSock->CreateSocksFD() && m_pSock->Connect() )
				{
					m_pSock->SetSkipConnect( true ); // this tells the socket that the connection state has been started
					bFound = true;
					break;
				}
				m_pSock->CloseSocksFD();
			}
			else if( bTryConnect )
			{
				bFound = true;
			}
		}

		if( bFound ) // the data pointed to here is invalid now, but the pointer itself is a good test
		{
			return( 0 );
		}
	}
	return( ETIMEDOUT );
}

int CS_GetAddrInfo( const CS_STRING & sHostname, Csock * pSock, CSSockAddr & csSockAddr )
{
#ifdef USE_GETHOSTBYNAME
	if( pSock )
		pSock->SetIPv6( false );
	csSockAddr.SetIPv6( false );
	int iRet = __GetHostByName( sHostname, csSockAddr.GetAddr(), 3 );
	return( iRet );
#else
	CGetAddrInfo cInfo( sHostname, pSock, csSockAddr );
	cInfo.Init();
	int iRet = cInfo.Process();
	if( iRet != 0 )
		return( iRet );
	return( cInfo.Finish() );
#endif /* USE_GETHOSTBYNAME */
}

int Csock::ConvertAddress( const struct sockaddr_storage * pAddr, socklen_t iAddrLen, CS_STRING & sIP, uint16_t * piPort ) const
{
	char szHostname[NI_MAXHOST];
	char szServ[NI_MAXSERV];
	int iRet = getnameinfo( ( const struct sockaddr * )pAddr, iAddrLen, szHostname, NI_MAXHOST, szServ, NI_MAXSERV, NI_NUMERICHOST|NI_NUMERICSERV );
	if( iRet == 0 )
	{
		sIP = szHostname;
		if( piPort )
			*piPort = ( uint16_t )atoi( szServ );
	}
	return( iRet );
}

bool InitCsocket()
{
#ifdef _WIN32
	WSADATA wsaData;
	int iResult = WSAStartup( MAKEWORD( 2, 2 ), &wsaData );
	if( iResult != NO_ERROR )
		return( false );
#endif /* _WIN32 */
#ifdef HAVE_C_ARES
#if ARES_VERSION >= CREATE_ARES_VER( 1, 6, 1 )
	if( ares_library_init( ARES_LIB_INIT_ALL ) != 0 )
		return( false );
#endif /* ARES_VERSION >= CREATE_ARES_VER( 1, 6, 1 ) */
#endif /* HAVE_C_ARES */
#ifdef HAVE_LIBSSL
	if( !InitSSL() )
		return( false );
#endif /* HAVE_LIBSSL */
	return( true );
}

void ShutdownCsocket()
{
#ifdef HAVE_LIBSSL
#if defined( HAVE_ERR_REMOVE_THREAD_STATE )
	ERR_remove_thread_state( NULL );
#elif defined( HAVE_ERR_REMOVE_STATE )
	ERR_remove_state( 0 );
#endif
#ifndef OPENSSL_NO_ENGINE
	ENGINE_cleanup();
#endif
#ifndef OPENSSL_IS_BORINGSSL
	CONF_modules_unload( 1 );
#endif
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
#endif /* HAVE_LIBSSL */
#ifdef HAVE_C_ARES
#if ARES_VERSION >= CREATE_ARES_VER( 1, 6, 1 )
	ares_library_cleanup();
#endif /* ARES_VERSION >= CREATE_ARES_VER( 1, 6, 1 ) */
#endif /* HAVE_C_ARES */
#ifdef _WIN32
	WSACleanup();
#endif /* _WIN32 */
}

#ifdef HAVE_LIBSSL
bool InitSSL( ECompType eCompressionType )
{
	SSL_load_error_strings();
	if( SSL_library_init() != 1 )
	{
		CS_DEBUG( "SSL_library_init() failed!" );
		return( false );
	}

#ifndef _WIN32
	if( access( "/dev/urandom", R_OK ) == 0 )
	{
		RAND_load_file( "/dev/urandom", 1024 );
	}
	else if( access( "/dev/random", R_OK ) == 0 )
	{
		RAND_load_file( "/dev/random", 1024 );
	}
	else
	{
		CS_DEBUG( "Unable to locate entropy location! Tried /dev/urandom and /dev/random" );
		return( false );
	}
#endif /* _WIN32 */

#ifndef OPENSSL_NO_COMP
	COMP_METHOD *cm = NULL;

	if( CT_ZLIB & eCompressionType )
	{
		cm = COMP_zlib();
		if( cm )
			SSL_COMP_add_compression_method( CT_ZLIB, cm );
	}
#endif

	// setting this up once in the begining
	s_iCsockSSLIdx = SSL_get_ex_new_index( 0, NULL, NULL, NULL, NULL );

	return( true );
}

void SSLErrors( const char *filename, u_int iLineNum )
{
	unsigned long iSSLError = 0;
	while( ( iSSLError = ERR_get_error() ) != 0 )
	{
		CS_DEBUG( "at " << filename << ":" << iLineNum );
		char szError[512];
		memset( ( char * ) szError, '\0', 512 );
		ERR_error_string_n( iSSLError, szError, 511 );
		if( *szError )
			CS_DEBUG( szError );
	}
}
#endif /* HAVE_LIBSSL */

void CSAdjustTVTimeout( struct timeval & tv, long iTimeoutMS )
{
	if( iTimeoutMS >= 0 )
	{
		long iCurTimeout = tv.tv_usec / 1000;
		iCurTimeout += tv.tv_sec * 1000;
		if( iCurTimeout > iTimeoutMS )
		{
			tv.tv_sec = iTimeoutMS / 1000;
			tv.tv_usec = iTimeoutMS % 1000;
		}
	}
}

#define CS_UNKNOWN_ERROR "Unknown Error"
static const char * CS_StrError( int iErrno, char * pszBuff, size_t uBuffLen )
{
#if defined( sgi ) || defined(__sun) || (defined(__NetBSD_Version__) && __NetBSD_Version__ < 4000000000)
	return( strerror( iErrno ) );
#else
	memset( pszBuff, '\0', uBuffLen );
#if defined( _WIN32 )
	if ( strerror_s( pszBuff, uBuffLen, iErrno ) == 0 )
		return( pszBuff );
#elif !defined( _GNU_SOURCE ) || !defined(__GLIBC__) || defined( __FreeBSD__ )
	if( strerror_r( iErrno, pszBuff, uBuffLen ) == 0 )
		return( pszBuff );
#else
	return( strerror_r( iErrno, pszBuff, uBuffLen ) );
#endif /* (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && !defined( _GNU_SOURCE ) */
#endif /* defined( sgi ) || defined(__sun) || defined(_WIN32) || (defined(__NetBSD_Version__) && __NetBSD_Version__ < 4000000000) */
	return( CS_UNKNOWN_ERROR );
}

void __Perror( const CS_STRING & s, const char * pszFile, u_int iLineNo )
{
	char szBuff[0xff];
	std::cerr << s << "(" << pszFile << ":" << iLineNo << "): " << CS_StrError( GetSockError(), szBuff, 0xff ) << endl;
}

static uint64_t millitime()
{
	std::chrono::time_point< std::chrono::steady_clock > time = std::chrono::steady_clock::now();
	return std::chrono::duration_cast< std::chrono::milliseconds >( time.time_since_epoch() ).count();
}

#define CS_GETTIMEOFDAY cxx11_gettimeofday
static int cxx11_gettimeofday( struct timeval * now, void * )
{
	std::chrono::time_point< std::chrono::steady_clock > time = std::chrono::steady_clock::now();
	long long micros = std::chrono::duration_cast< std::chrono::microseconds >( time.time_since_epoch() ).count();
	now->tv_sec = micros / 1000000LL;
	now->tv_usec = micros % 1000000LL;
	return 0;
}

#ifndef _NO_CSOCKET_NS // some people may not want to use a namespace
}
using namespace Csocket;
#endif /* _NO_CSOCKET_NS */

CCron::CCron()
{
	m_iCycles = 0;
	m_iMaxCycles = 0;
	m_bActive = true;
	timerclear( &m_tTime );
	m_tTimeSequence.tv_sec = 60;
	m_tTimeSequence.tv_usec = 0;
	m_bPause = false;
	m_bRunOnNextCall = false;
}

void CCron::run( timeval & tNow )
{
	if( m_bPause )
		return;

	if( !timerisset( &tNow ) )
		CS_GETTIMEOFDAY( &tNow, NULL );

	if( m_bActive && ( !timercmp( &tNow, &m_tTime, < ) || m_bRunOnNextCall ) )
	{
		m_bRunOnNextCall = false; // Setting this here because RunJob() could set it back to true
		RunJob();

		if( m_iMaxCycles > 0 && ++m_iCycles >= m_iMaxCycles )
			m_bActive = false;
		else
			timeradd( &tNow, &m_tTimeSequence, &m_tTime );
	}
}

void CCron::StartMaxCycles( double dTimeSequence, u_int iMaxCycles )
{
	timeval tNow;
	m_tTimeSequence.tv_sec = ( time_t ) dTimeSequence;
	// this could be done with modf(), but we're avoiding bringing in libm just for the one function.
	m_tTimeSequence.tv_usec = ( suseconds_t )( ( dTimeSequence - ( double )( ( time_t ) dTimeSequence ) ) * 1000000 );
	CS_GETTIMEOFDAY( &tNow, NULL );
	timeradd( &tNow, &m_tTimeSequence, &m_tTime );
	m_iMaxCycles = iMaxCycles;
	m_bActive = true;
}

void CCron::StartMaxCycles( const timeval& tTimeSequence, u_int iMaxCycles )
{
	timeval tNow;
	m_tTimeSequence = tTimeSequence;
	CS_GETTIMEOFDAY( &tNow, NULL );
	timeradd( &tNow, &m_tTimeSequence, &m_tTime );
	m_iMaxCycles = iMaxCycles;
	m_bActive = true;
}

void CCron::Start( double dTimeSequence )
{
	StartMaxCycles( dTimeSequence, 0 );
}

void CCron::Start( const timeval& tTimeSequence )
{
	StartMaxCycles( tTimeSequence, 0 );
}

void CCron::Stop()
{
	m_bActive = false;
}

void CCron::Pause()
{
	m_bPause = true;
}

void CCron::UnPause()
{
	m_bPause = false;
}

void CCron::Reset()
{
	Stop();
	Start(m_tTimeSequence);
}

timeval CCron::GetInterval() const { return( m_tTimeSequence ); }
u_int CCron::GetMaxCycles() const { return( m_iMaxCycles ); }
u_int CCron::GetCyclesLeft() const { return( ( m_iMaxCycles > m_iCycles ? ( m_iMaxCycles - m_iCycles ) : 0 ) ); }

bool CCron::isValid() const { return( m_bActive ); }
const CS_STRING & CCron::GetName() const { return( m_sName ); }
void CCron::SetName( const CS_STRING & sName ) { m_sName = sName; }
void CCron::RunJob() { CS_DEBUG( "This should be overridden" ); }

bool CSMonitorFD::GatherFDsForSelect( std::map< cs_sock_t, short > & miiReadyFds, long & iTimeoutMS )
{
	iTimeoutMS = -1; // don't bother changing anything in the default implementation
	for( std::map< cs_sock_t, short >::iterator it = m_miiMonitorFDs.begin(); it != m_miiMonitorFDs.end(); ++it )
	{
		miiReadyFds[it->first] = it->second;
	}
	return( m_bEnabled );
}

bool CSMonitorFD::CheckFDs( const std::map< cs_sock_t, short > & miiReadyFds )
{
	std::map< cs_sock_t, short > miiTriggerdFds;
	for( std::map< cs_sock_t, short >::iterator it = m_miiMonitorFDs.begin(); it != m_miiMonitorFDs.end(); ++it )
	{
		std::map< cs_sock_t, short >::const_iterator itFD = miiReadyFds.find( it->first );
		if( itFD != miiReadyFds.end() )
			miiTriggerdFds[itFD->first] = itFD->second;
	}
	if( !miiTriggerdFds.empty() )
		return( FDsThatTriggered( miiTriggerdFds ) );
	return( m_bEnabled );
}

CSockCommon::~CSockCommon()
{
	// delete any left over crons
	CleanupCrons();
	CleanupFDMonitors();
}

void CSockCommon::CleanupCrons()
{
	for( size_t a = 0; a < m_vcCrons.size(); ++a )
		CS_Delete( m_vcCrons[a] );
	m_vcCrons.clear();
}

void CSockCommon::CleanupFDMonitors()
{
	for( size_t a = 0; a < m_vcMonitorFD.size(); ++a )
		CS_Delete( m_vcMonitorFD[a] );
	m_vcMonitorFD.clear();
}

void CSockCommon::CheckFDs( const std::map< cs_sock_t, short > & miiReadyFds )
{
	for( size_t uMon = 0; uMon < m_vcMonitorFD.size(); ++uMon )
	{
		if( !m_vcMonitorFD[uMon]->IsEnabled() || !m_vcMonitorFD[uMon]->CheckFDs( miiReadyFds ) )
			m_vcMonitorFD.erase( m_vcMonitorFD.begin() + uMon-- );
	}
}

void CSockCommon::AssignFDs( std::map< cs_sock_t, short > & miiReadyFds, struct timeval * tvtimeout )
{
	for( size_t uMon = 0; uMon < m_vcMonitorFD.size(); ++uMon )
	{
		long iTimeoutMS = -1;
		if( m_vcMonitorFD[uMon]->IsEnabled() && m_vcMonitorFD[uMon]->GatherFDsForSelect( miiReadyFds, iTimeoutMS ) )
		{
			CSAdjustTVTimeout( *tvtimeout, iTimeoutMS );
		}
		else
		{
			CS_Delete( m_vcMonitorFD[uMon] );
			m_vcMonitorFD.erase( m_vcMonitorFD.begin() + uMon-- );
		}
	}
}


void CSockCommon::Cron()
{
	timeval tNow;
	timerclear( &tNow );

	for( vector<CCron *>::size_type a = 0; a < m_vcCrons.size(); ++a )
	{
		CCron * pcCron = m_vcCrons[a];

		if( !pcCron->isValid() )
		{
			CS_Delete( pcCron );
			m_vcCrons.erase( m_vcCrons.begin() + a-- );
		}
		else
		{
			pcCron->run( tNow );
		}
	}
}

void CSockCommon::AddCron( CCron * pcCron )
{
	m_vcCrons.push_back( pcCron );
}

void CSockCommon::DelCron( const CS_STRING & sName, bool bDeleteAll, bool bCaseSensitive )
{
	for( size_t a = 0; a < m_vcCrons.size(); ++a )
	{
		int ( *Cmp )( const char *, const char * ) = ( bCaseSensitive ? strcmp : strcasecmp );
		if( Cmp( m_vcCrons[a]->GetName().c_str(), sName.c_str() ) == 0 )
		{
			m_vcCrons[a]->Stop();
			CS_Delete( m_vcCrons[a] );
			m_vcCrons.erase( m_vcCrons.begin() + a-- );
			if( !bDeleteAll )
				break;
		}
	}
}

void CSockCommon::DelCron( u_int iPos )
{
	if( iPos < m_vcCrons.size() )
	{
		m_vcCrons[iPos]->Stop();
		CS_Delete( m_vcCrons[iPos] );
		m_vcCrons.erase( m_vcCrons.begin() + iPos );
	}
}

void CSockCommon::DelCronByAddr( CCron * pcCron )
{
	for( size_t a = 0; a < m_vcCrons.size(); ++a )
	{
		if( m_vcCrons[a] == pcCron )
		{
			m_vcCrons[a]->Stop();
			CS_Delete( m_vcCrons[a] );
			m_vcCrons.erase( m_vcCrons.begin() + a );
			return;
		}
	}
}

Csock::Csock( int iTimeout ) : CSockCommon()
{
#ifdef HAVE_LIBSSL
	m_pCerVerifyCB = _CertVerifyCB;
#endif /* HAVE_LIBSSL */
	Init( "", 0, iTimeout );
}

Csock::Csock( const CS_STRING & sHostname, uint16_t iport, int iTimeout ) : CSockCommon()
{
#ifdef HAVE_LIBSSL
	m_pCerVerifyCB = _CertVerifyCB;
#endif /* HAVE_LIBSSL */
	Init( sHostname, iport, iTimeout );
}

// override this for accept sockets
Csock *Csock::GetSockObj( const CS_STRING & sHostname, uint16_t iPort )
{
	return( NULL );
}

#ifdef HAVE_LIBSSL
bool Csock::SNIConfigureClient( CS_STRING & sHostname )
{
	if( m_shostname.empty() )
		return( false );
	sHostname = m_shostname;
	return( true );
}
#endif

#ifdef _WIN32
#define CS_CLOSE closesocket
#else
#define CS_CLOSE close
#endif /* _WIN32 */

Csock::~Csock()
{
#ifdef _WIN32
	// prevent successful closesocket() calls and such from
	// overwriting any possible previous errors.
	int iOldError = ::WSAGetLastError();
#endif /* _WIN32 */

#ifdef HAVE_ICU
	if( m_cnvExt ) ucnv_close( m_cnvExt );
	if( m_cnvInt ) ucnv_close( m_cnvInt );
#endif

#ifdef HAVE_C_ARES
	if( m_pARESChannel )
		ares_cancel( m_pARESChannel );
	FreeAres();
#endif /* HAVE_C_ARES */

#ifdef HAVE_LIBSSL
	FREE_SSL();
	FREE_CTX();
#endif /* HAVE_LIBSSL */

	CloseSocksFD();

#ifdef HAVE_UNIX_SOCKET
	if( m_bUnixListen )
		::unlink(m_sBindHost.c_str());
#endif

#ifdef _WIN32
	::WSASetLastError( iOldError );
#endif /* _WIN32 */
}

void Csock::CloseSocksFD()
{
	if( m_iReadSock != m_iWriteSock )
	{
		if( m_iReadSock != CS_INVALID_SOCK )
			CS_CLOSE( m_iReadSock );
		if( m_iWriteSock != CS_INVALID_SOCK )
			CS_CLOSE( m_iWriteSock );
	}
	else if( m_iReadSock != CS_INVALID_SOCK )
	{
		CS_CLOSE( m_iReadSock );
	}

	m_iReadSock = CS_INVALID_SOCK;
	m_iWriteSock = CS_INVALID_SOCK;
}


void Csock::Dereference()
{
	m_iWriteSock = m_iReadSock = CS_INVALID_SOCK;

#ifdef HAVE_LIBSSL
	m_ssl = NULL;
	m_ssl_ctx = NULL;
#endif /* HAVE_LIBSSL */

	// don't delete and erase, just erase since they were moved to the copied sock
	m_vcCrons.clear();
	m_vcMonitorFD.clear();
	Close( CLT_DEREFERENCE );
}

void Csock::Copy( const Csock & cCopy )
{
	m_iTcount		= cCopy.m_iTcount;
	m_iLastCheckTimeoutTime	=	cCopy.m_iLastCheckTimeoutTime;
	m_uPort 		= cCopy.m_uPort;
	m_iRemotePort	= cCopy.m_iRemotePort;
	m_iLocalPort	= cCopy.m_iLocalPort;
	m_iReadSock		= cCopy.m_iReadSock;
	m_iWriteSock	= cCopy.m_iWriteSock;
	m_iTimeout		= cCopy.m_iTimeout;
	m_iMaxConns		= cCopy.m_iMaxConns;
	m_iConnType		= cCopy.m_iConnType;
	m_iMethod		= cCopy.m_iMethod;
	m_bUseSSL			= cCopy.m_bUseSSL;
	m_bIsConnected	= cCopy.m_bIsConnected;
	m_bsslEstablished	= cCopy.m_bsslEstablished;
	m_bEnableReadLine	= cCopy.m_bEnableReadLine;
	m_bPauseRead		= cCopy.m_bPauseRead;
	m_shostname		= cCopy.m_shostname;
	m_sbuffer		= cCopy.m_sbuffer;
	m_sSockName		= cCopy.m_sSockName;
	m_sKeyFile		= cCopy.m_sKeyFile;
	m_sDHParamFile		= cCopy.m_sDHParamFile;
	m_sPemFile		= cCopy.m_sPemFile;
	m_sCipherType	= cCopy.m_sCipherType;
	m_sParentName	= cCopy.m_sParentName;
	m_sSend			= cCopy.m_sSend;
	m_sPemPass		= cCopy.m_sPemPass;
	m_sLocalIP		= cCopy.m_sLocalIP;
	m_sRemoteIP		= cCopy.m_sRemoteIP;
	m_eCloseType	= cCopy.m_eCloseType;

	m_iMaxMilliSeconds	= cCopy.m_iMaxMilliSeconds;
	m_iLastSendTime		= cCopy.m_iLastSendTime;
	m_iBytesRead		= cCopy.m_iBytesRead;
	m_iBytesWritten		= cCopy.m_iBytesWritten;
	m_iStartTime		= cCopy.m_iStartTime;
	m_iMaxBytes			= cCopy.m_iMaxBytes;
	m_iLastSend			= cCopy.m_iLastSend;
	m_uSendBufferPos	= cCopy.m_uSendBufferPos;
	m_iMaxStoredBufferLength	= cCopy.m_iMaxStoredBufferLength;
	m_iTimeoutType		= cCopy.m_iTimeoutType;

	m_address			= cCopy.m_address;
	m_bindhost			= cCopy.m_bindhost;
	m_bIsIPv6			= cCopy.m_bIsIPv6;
	m_bSkipConnect		= cCopy.m_bSkipConnect;
#ifdef HAVE_C_ARES
	FreeAres(); // Not copying this state, but making sure its nulled out
	m_iARESStatus = -1; // set it to unitialized
	m_pCurrAddr = NULL;
#endif /* HAVE_C_ARES */

#ifdef HAVE_LIBSSL
	m_bNoSSLCompression = cCopy.m_bNoSSLCompression;
	m_bSSLCipherServerPreference = cCopy.m_bSSLCipherServerPreference;
	m_uDisableProtocols = cCopy.m_uDisableProtocols;
	m_iRequireClientCertFlags = cCopy.m_iRequireClientCertFlags;
	m_sSSLBuffer	= cCopy.m_sSSLBuffer;

	FREE_SSL();
	FREE_CTX(); // be sure to remove anything that was already here
	m_ssl				= cCopy.m_ssl;
	m_ssl_ctx			= cCopy.m_ssl_ctx;

	m_pCerVerifyCB		= cCopy.m_pCerVerifyCB;

	if( m_ssl )
	{
		SSL_set_ex_data( m_ssl, GetCsockSSLIdx(), this );
#if defined( SSL_CTX_set_tlsext_servername_callback )
		SSL_CTX_set_tlsext_servername_arg( m_ssl_ctx, this );
#endif /* SSL_CTX_set_tlsext_servername_callback */
	}

#endif /* HAVE_LIBSSL */
#ifdef HAVE_UNIX_SOCKET
	m_bUnixListen = cCopy.m_bUnixListen;
#endif

#ifdef HAVE_ICU
	SetEncoding(cCopy.m_sEncoding);
#endif

	CleanupCrons();
	CleanupFDMonitors();
	m_vcCrons			= cCopy.m_vcCrons;
	m_vcMonitorFD		= cCopy.m_vcMonitorFD;

	m_eConState			= cCopy.m_eConState;
	m_sBindHost			= cCopy.m_sBindHost;
	m_iCurBindCount		= cCopy.m_iCurBindCount;
	m_iDNSTryCount		= cCopy.m_iDNSTryCount;

}

Csock & Csock::operator<<( const CS_STRING & s )
{
	Write( s );
	return( *this );
}

Csock & Csock::operator<<( ostream & ( *io )( ostream & ) )
{
	Write( "\r\n" );
	return( *this );
}

Csock & Csock::operator<<( int32_t i )
{
	stringstream s;
	s << i;
	Write( s.str() );
	return( *this );
}

Csock & Csock::operator<<( uint32_t i )
{
	stringstream s;
	s << i;
	Write( s.str() );
	return( *this );
}

Csock & Csock::operator<<( int64_t i )
{
	stringstream s;
	s << i;
	Write( s.str() );
	return( *this );
}

Csock & Csock::operator<<( uint64_t i )
{
	stringstream s;
	s << i;
	Write( s.str() );
	return( *this );
}

Csock & Csock::operator<<( float i )
{
	stringstream s;
	s << i;
	Write( s.str() );
	return( *this );
}

Csock & Csock::operator<<( double i )
{
	stringstream s;
	s << i;
	Write( s.str() );
	return( *this );
}

bool Csock::Connect()
{
	if( m_bSkipConnect )
	{
		// this was already called, so skipping now. this is to allow easy pass through
		if( m_eConState != CST_OK )
		{
			m_eConState = CST_CONNECTWAIT;
		}
		return( true );
	}

#ifndef _WIN32
	set_non_blocking( m_iReadSock );
#else
	if( !GetIPv6() )
		set_non_blocking( m_iReadSock );
	// non-blocking sockets on Win32 do *not* return ENETUNREACH/EHOSTUNREACH if there's no IPv6 gateway.
	// we need those error codes for the v4 fallback in GetAddrInfo!
#endif  /* _WIN32 */

	m_iConnType = OUTBOUND;

	int ret = -1;
	if( !GetIPv6() )
		ret = connect( m_iReadSock, ( struct sockaddr * )m_address.GetSockAddr(), m_address.GetSockAddrLen() );
#ifdef HAVE_IPV6
	else
		ret = connect( m_iReadSock, ( struct sockaddr * )m_address.GetSockAddr6(), m_address.GetSockAddrLen6() );
#endif /* HAVE_IPV6 */
#ifndef _WIN32
	if( ret == -1 && GetSockError() != EINPROGRESS )
#else
	if( ret == -1 && GetSockError() != EINPROGRESS && GetSockError() != WSAEWOULDBLOCK )
#endif /* _WIN32 */

	{
		CS_DEBUG( "Connect Failed. ERRNO [" << GetSockError() << "] FD [" << m_iReadSock << "]" );
		return( false );
	}

#ifdef _WIN32
	// do what we didn't do above since connect() is now over!
	if( GetIPv6() )
		set_non_blocking( m_iReadSock );
#endif /* _WIN32 */

	if( m_eConState != CST_OK )
	{
		m_eConState = CST_CONNECTWAIT;
	}

	return( true );
}


#ifdef HAVE_UNIX_SOCKET
static bool prepare_sockaddr(struct sockaddr_un * addr, const CS_STRING & sPath)
{
	if ( sPath.empty() )
		return( false );

	memset( addr, 0, sizeof(*addr) );
	addr->sun_family = AF_UNIX;
	auto length = sPath.length();
	if( sizeof(addr->sun_path) <= length )
		return( false );
	memcpy( &addr->sun_path, sPath.c_str(), length + 1 );
	// Linux abstract namespace is null followed by name.
	if (sPath[0] == '@') addr->sun_path[0] = 0;
	return true;
}

bool Csock::ConnectUnixInternal( const CS_STRING & sPath )
{
	if( m_iReadSock != m_iWriteSock )
		return( false );
	if( m_iReadSock == CS_INVALID_SOCK )
		m_iReadSock = m_iWriteSock = CreateSocket( false, true );

#ifndef __CYGWIN__
	// Cygwin emulates unix sockets using IP sockets, so connect() actually
	// needs to be blocking, otherwise it returns SockError(119 Operation now in
	// progress). Therefore for cygwin do it after connect().
	set_non_blocking( m_iReadSock );
#endif
	m_iConnType = OUTBOUND;

	struct sockaddr_un addr;
	if( !prepare_sockaddr( &addr, sPath) )
	{
		CallSockError( EADDRNOTAVAIL );
		return( false );
	}
	if( connect( m_iReadSock, ( struct sockaddr * )&addr, sizeof(addr) ) == -1)
	{
		int e = GetSockError();
		CS_DEBUG( "Connect Failed. ERRNO [" << e << "] FD [" << m_iReadSock << "]" );
		if( e == ECONNREFUSED )
			ConnectionRefused();
		else
			CallSockError( e );
		return( false );
	}

#ifdef __CYGWIN__
	set_non_blocking( m_iReadSock );
#endif
	m_eConState = ( GetSSL() ? CST_CONNECTSSL : CST_OK );

	return( true );
}

bool Csock::ListenUnixInternal( const CS_STRING & sBindFile, int iMaxConns, u_int iTimeout )
{
	m_iConnType = LISTENER;
	m_iTimeout = iTimeout;
	m_sBindHost = sBindFile;
	m_iMaxConns = iMaxConns;

	SetConState( Csock::CST_OK );

	// Should m_address be set up somehow?

	struct sockaddr_un addr;
	if( !prepare_sockaddr( &addr, sBindFile) )
	{
		CallSockError( EADDRNOTAVAIL );
		CS_DEBUG( "Csock::ListenUnixInternal(): prepare_sockaddr failed" );
		return( false );
	}

	m_iReadSock = m_iWriteSock = CreateSocket( true, true );

	if( m_iReadSock == CS_INVALID_SOCK )
	{
		CallSockError( EBADF );
		CS_DEBUG( "Csock::ListenUnixInternal(): CreateSocket failed" );
		return( false );
	}

	if( bind( m_iReadSock, ( struct sockaddr * ) &addr, sizeof(addr) ) == -1 )
	{
		CallSockError( GetSockError() );
		CS_DEBUG( "Csock::ListenUnixInternal(): bind failed" );
		return( false );
	}

	if( listen( m_iReadSock, iMaxConns ) == -1 )
	{
		CallSockError( GetSockError() );
		CS_DEBUG( "Csock::ListenUnixInternal(): listen failed" );
		return( false );
	}

	// set it none blocking
	set_non_blocking( m_iReadSock );

	// TODO: The following callback makes no sense here; should a
	// ListeningUnix() be added? We aren't doing anything asynchronous...
	//Listening( m_sBindHost, m_uPort );

	m_bUnixListen = true;
	return( true );
}
#endif

bool Csock::Listen( uint16_t iPort, int iMaxConns, const CS_STRING & sBindHost, u_int iTimeout, bool bDetach )
{
	m_iConnType = LISTENER;
	m_iTimeout = iTimeout;
	m_sBindHost = sBindHost;
	m_iMaxConns = iMaxConns;

	SetConState( Csock::CST_OK );
	if( !m_sBindHost.empty() )
	{
		if( bDetach )
		{
			int iRet = GetAddrInfo( m_sBindHost, m_address );
			if( iRet == ETIMEDOUT )
			{
				CallSockError( EADDRNOTAVAIL );
				return( false );
			}
			else if( iRet == EAGAIN )
			{
				SetConState( Csock::CST_BINDVHOST );
				return( true );
			}
		}
		else
		{
			// if not detaching, then must block to do DNS resolution, so might as well use internal resolver
			if( ::CS_GetAddrInfo( m_sBindHost, this, m_address ) != 0 )
			{
				CallSockError( EADDRNOTAVAIL );
				return( false );
			}
		}
	}

	m_iReadSock = m_iWriteSock = CreateSocket( true );

	if( m_iReadSock == CS_INVALID_SOCK )
	{
		CallSockError( EBADF );
		return( false );
	}

#ifdef HAVE_IPV6
# ifdef _WIN32
#  ifndef IPPROTO_IPV6
#   define IPPROTO_IPV6 41 /* define for apps with _WIN32_WINNT < 0x0501 (XP) */
#  endif /* !IPPROTO_IPV6 */
#  ifndef IPV6_V6ONLY
#   define IPV6_V6ONLY 27
#  endif
	/* check for IPV6_V6ONLY support at runtime: only supported on Windows Vista or later */
	OSVERSIONINFOEX osvi = { 0 };
	DWORDLONG dwlConditionMask = 0;

	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	osvi.dwMajorVersion = 6;

	VER_SET_CONDITION(dwlConditionMask, VER_MAJORVERSION, VER_GREATER_EQUAL);

	if( VerifyVersionInfo( &osvi, VER_MAJORVERSION, dwlConditionMask ) )
	{
# endif /* _WIN32 */
# ifdef IPV6_V6ONLY
		if( GetIPv6() )
		{
			// per RFC3493#5.3
			const int on = ( m_address.GetAFRequire() == CSSockAddr::RAF_INET6 ? 1 : 0 );
			if( setsockopt( m_iReadSock, IPPROTO_IPV6, IPV6_V6ONLY, ( char * )&on, sizeof( on ) ) != 0 )
				PERROR( "IPV6_V6ONLY" );
		}
# endif /* IPV6_V6ONLY */
# ifdef _WIN32
	}
# endif /* _WIN32 */
#endif /* HAVE_IPV6 */

	m_address.SinFamily();
	m_address.SinPort( iPort );
	if( !GetIPv6() )
	{
		if( bind( m_iReadSock, ( struct sockaddr * ) m_address.GetSockAddr(), m_address.GetSockAddrLen() ) == -1 )
		{
			CallSockError( GetSockError() );
			return( false );
		}
	}
#ifdef HAVE_IPV6
	else
	{
		if( bind( m_iReadSock, ( struct sockaddr * ) m_address.GetSockAddr6(), m_address.GetSockAddrLen6() ) == -1 )
		{
			CallSockError( GetSockError() );
			return( false );
		}
	}
#endif /* HAVE_IPV6 */

	if( listen( m_iReadSock, iMaxConns ) == -1 )
	{
		CallSockError( GetSockError() );
		return( false );
	}

	// set it none blocking
	set_non_blocking( m_iReadSock );
	if( m_uPort == 0 || !m_sBindHost.empty() )
	{
		struct sockaddr_storage cAddr;
		socklen_t iAddrLen = sizeof( cAddr );
		if( getsockname( m_iReadSock, ( struct sockaddr * )&cAddr, &iAddrLen ) == 0 )
		{
			ConvertAddress( &cAddr, iAddrLen, m_sBindHost, &m_uPort );
		}
	}
	Listening( m_sBindHost, m_uPort );

	return( true );
}

cs_sock_t Csock::Accept( CS_STRING & sHost, uint16_t & iRPort )
{
	cs_sock_t iSock = CS_INVALID_SOCK;
	struct sockaddr_storage cAddr;
	socklen_t iAddrLen = sizeof( cAddr );
	iSock = accept( m_iReadSock, ( struct sockaddr * )&cAddr, &iAddrLen );
	if( iSock != CS_INVALID_SOCK && getpeername( iSock, ( struct sockaddr * )&cAddr, &iAddrLen ) == 0 )
	{
		ConvertAddress( &cAddr, iAddrLen, sHost, &iRPort );
	}

	if( iSock != CS_INVALID_SOCK )
	{
		// Make it close-on-exec
		set_close_on_exec( iSock );

		// make it none blocking
		set_non_blocking( iSock );

		if( !ConnectionFrom( sHost, iRPort ) )
		{
			CS_CLOSE( iSock );
			iSock = CS_INVALID_SOCK;
		}

	}

	return( iSock );
}

#ifdef HAVE_LIBSSL
#if defined( SSL_CTX_set_tlsext_servername_callback )
static int __SNICallBack( SSL *pSSL, int *piAD, void *pData ) 
{
	if( !pSSL || !pData )
		return( SSL_TLSEXT_ERR_NOACK );

	const char * pServerName = SSL_get_servername( pSSL, TLSEXT_NAMETYPE_host_name );
	if( !pServerName )
		return( SSL_TLSEXT_ERR_NOACK );

	Csock * pSock = static_cast<Csock *>( pData );

	CS_STRING sDHParamFile, sKeyFile, sPemFile, sPemPass;
	if( !pSock->SNIConfigureServer( pServerName, sPemFile, sPemPass ) )
		return( SSL_TLSEXT_ERR_NOACK );

	pSock->SetDHParamLocation( sDHParamFile );
	pSock->SetKeyLocation( sKeyFile );
	pSock->SetPemLocation( sPemFile );
	pSock->SetPemPass( sPemPass );
	SSL_CTX * pCTX = pSock->SetupServerCTX();
	SSL_set_SSL_CTX( pSSL, pCTX );
	pSock->SetCTXObject( pCTX, true );
	return( SSL_TLSEXT_ERR_OK );
}
#endif /* SSL_CTX_set_tlsext_servername_callback */
#endif /* HAVE_LIBSSL */

bool Csock::AcceptSSL()
{
#ifdef HAVE_LIBSSL
	if( !m_ssl )
		if( !SSLServerSetup() )
			return( false );

#if defined( SSL_CTX_set_tlsext_servername_callback )
	SSL_CTX_set_tlsext_servername_callback( m_ssl_ctx, __SNICallBack );
	SSL_CTX_set_tlsext_servername_arg( m_ssl_ctx, this );
#endif /* SSL_CTX_set_tlsext_servername_callback */

	int err = SSL_accept( m_ssl );

	if( err == 1 )
	{
		return( true );
	}

	int sslErr = SSL_get_error( m_ssl, err );

	if( sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE )
		return( true );

	SSLErrors( __FILE__, __LINE__ );

#endif /* HAVE_LIBSSL */

	return( false );
}

#ifdef HAVE_LIBSSL
bool Csock::ConfigureCTXOptions( SSL_CTX * pCTX )
{
	if( pCTX )
	{
		if( SSL_CTX_set_cipher_list( pCTX, m_sCipherType.c_str() ) <= 0 )
		{
			CS_DEBUG( "Could not assign cipher [" << m_sCipherType << "]" );
			return( false );
		}

		long uCTXOptions = 0;
		if( m_uDisableProtocols > 0 )
		{
#ifdef SSL_OP_NO_SSLv2
			if( EDP_SSLv2 & m_uDisableProtocols )
				uCTXOptions |= SSL_OP_NO_SSLv2;
#endif /* SSL_OP_NO_SSLv2 */
#ifdef SSL_OP_NO_SSLv3
			if( EDP_SSLv3 & m_uDisableProtocols )
				uCTXOptions |= SSL_OP_NO_SSLv3;
#endif /* SSL_OP_NO_SSLv3 */
#ifdef SSL_OP_NO_TLSv1
			if( EDP_TLSv1 & m_uDisableProtocols )
				uCTXOptions |= SSL_OP_NO_TLSv1;
#endif /* SSL_OP_NO_TLSv1 */
#ifdef SSL_OP_NO_TLSv1_1
			if( EDP_TLSv1_1 & m_uDisableProtocols )
				uCTXOptions |= SSL_OP_NO_TLSv1_1;
#endif /* SSL_OP_NO_TLSv1 */
#ifdef SSL_OP_NO_TLSv1_2
			if( EDP_TLSv1_2 & m_uDisableProtocols )
				uCTXOptions |= SSL_OP_NO_TLSv1_2;
#endif /* SSL_OP_NO_TLSv1_2 */
		}
#ifdef SSL_OP_NO_COMPRESSION
		if( m_bNoSSLCompression )
			uCTXOptions |= SSL_OP_NO_COMPRESSION;
#endif /* SSL_OP_NO_COMPRESSION */
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
		if( m_bSSLCipherServerPreference )
			uCTXOptions |= SSL_OP_CIPHER_SERVER_PREFERENCE;
#endif /* SSL_OP_CIPHER_SERVER_PREFERENCE */
		if( uCTXOptions )
			SSL_CTX_set_options( pCTX, uCTXOptions );
	}
	return true;
}
#endif /* HAVE_LIBSSL */


#ifdef HAVE_LIBSSL
static SSL_CTX * GetSSLCTX( int iMethod )
{
	const SSL_METHOD *pMethod = NULL;

#ifdef HAVE_FLEXIBLE_TLS_METHOD
	int iProtoVersion = 0;
	pMethod = TLS_method();
#else
	pMethod = SSLv23_method();
#endif // HAVE_FLEXIBLE_TLS_METHOD

	switch( iMethod )
	{
	case Csock::TLS:
		break; // defaults already set above, anything else can either match a case or fall through and use defaults anyway
#ifdef HAVE_FLEXIBLE_TLS_METHOD
#ifndef OPENSSL_NO_TLS1_2
	case Csock::TLS12:
		iProtoVersion = TLS1_2_VERSION;
		break;
#endif /* OPENSSL_NO_TLS1_2 */
#ifndef OPENSSL_NO_TLS1_1
	case Csock::TLS11:
		iProtoVersion = TLS1_1_VERSION;
		break;
#endif /* OPENSSL_NO_TLS1_1 */
#ifndef OPENSSL_NO_TLS1
	case Csock::TLS1:
		iProtoVersion = TLS1_VERSION;
		break;
#endif /* OPENSSL_NO_TLS1 */
#ifndef OPENSSL_NO_SSL3
	case Csock::SSL3:
		iProtoVersion = SSL3_VERSION;
		break;
#endif /* OPENSSL_NO_SSL3 */
#ifndef OPENSSL_NO_SSL2
	case Csock::SSL2:
		pMethod = SSLv2_method();
		break;
#endif /* OPENSSL_NO_SSL2 */


#else /* HAVE_FLEXIBLE_TLS_METHOD */


#ifndef OPENSSL_NO_TLS1_2
	case Csock::TLS12:
		pMethod = TLSv1_2_method();
		break;
#endif /* OPENSSL_NO_TLS1_2 */
#ifndef OPENSSL_NO_TLS1_1
	case Csock::TLS11:
		pMethod = TLSv1_1_method();
		break;
#endif /* OPENSSL_NO_TLS1_1 */
#ifndef OPENSSL_NO_TLS1
	case Csock::TLS1:
		pMethod = TLSv1_method();
		break;
#endif /* OPENSSL_NO_TLS1 */
#ifndef OPENSSL_NO_SSL3
	case Csock::SSL3:
		pMethod = SSLv3_method();
		break;
#endif /* OPENSSL_NO_SSL3 */
#ifndef OPENSSL_NO_SSL2
	case Csock::SSL2:
		pMethod = SSLv2_method();
		break;
#endif /* OPENSSL_NO_SSL2 */
#endif /* HAVE_FLEXIBLE_TLS_METHOD */

	default:
		CS_DEBUG( "WARNING: SSL Client Method other than SSLv23 specified, but has passed through" );
		break;
	}

	SSL_CTX * pCTX = SSL_CTX_new( pMethod );
	if( !pCTX )
	{
		CS_DEBUG( "WARNING: GetSSLCTX failed!" );
		return( NULL );
	}

#ifdef HAVE_FLEXIBLE_TLS_METHOD
	if( iProtoVersion )
	{
		SSL_CTX_set_min_proto_version( pCTX, iProtoVersion );
		SSL_CTX_set_max_proto_version( pCTX, iProtoVersion );
	}
#endif /* HAVE_FLEXIBLE_TLS_METHOD */

	return( pCTX );
}
#endif

bool Csock::SSLClientSetup()
{
#ifdef HAVE_LIBSSL
	m_bUseSSL = true;
	FREE_SSL();
	FREE_CTX();

#ifdef _WIN64
	if( m_iReadSock != ( int )m_iReadSock || m_iWriteSock != ( int )m_iWriteSock )
	{
		// sanity check the FD to be sure its compatible with openssl
		CS_DEBUG( "ERROR: sockfd larger than OpenSSL can handle" );
		return( false );
	}
#endif /* _WIN64 */

	m_ssl_ctx = GetSSLCTX( m_iMethod );
	if( !m_ssl_ctx )
	{
		CS_DEBUG( "WARNING: Failed to retrieve a valid ctx" );
		return( false );
	}

	SSL_CTX_set_default_verify_paths( m_ssl_ctx );

	if( !m_sPemFile.empty() )
	{
		// are we sending a client cerificate ?
		SSL_CTX_set_default_passwd_cb( m_ssl_ctx, _PemPassCB );
		SSL_CTX_set_default_passwd_cb_userdata( m_ssl_ctx, ( void * )this );

		//
		// set up the CTX
		if( SSL_CTX_use_certificate_file( m_ssl_ctx, m_sPemFile.c_str() , SSL_FILETYPE_PEM ) <= 0 )
		{
			CS_DEBUG( "Error with SSLCert file [" << m_sPemFile << "]" );
			SSLErrors( __FILE__, __LINE__ );
		}
        CS_STRING privKeyFile = m_sKeyFile.empty() ? m_sPemFile : m_sKeyFile;
		if( SSL_CTX_use_PrivateKey_file( m_ssl_ctx, privKeyFile.c_str(), SSL_FILETYPE_PEM ) <= 0 )
		{
			CS_DEBUG( "Error with SSLKey file [" << privKeyFile << "]" );
			SSLErrors( __FILE__, __LINE__ );
		}
	}

	if( !ConfigureCTXOptions( m_ssl_ctx ) )
	{
		SSL_CTX_free( m_ssl_ctx );
		m_ssl_ctx = NULL;
		return( false );
	}

	m_ssl = SSL_new( m_ssl_ctx );
	if( !m_ssl )
		return( false );

	SSL_set_rfd( m_ssl, ( int )m_iReadSock );
	SSL_set_wfd( m_ssl, ( int )m_iWriteSock );
	SSL_set_verify( m_ssl, SSL_VERIFY_PEER, m_pCerVerifyCB );
	SSL_set_info_callback( m_ssl, _InfoCallback );
	SSL_set_ex_data( m_ssl, GetCsockSSLIdx(), this );

#if defined( SSL_set_tlsext_host_name )
	CS_STRING sSNIHostname;
	if( SNIConfigureClient( sSNIHostname ) )
		SSL_set_tlsext_host_name( m_ssl, sSNIHostname.c_str() );
#endif /* SSL_set_tlsext_host_name */

	SSLFinishSetup( m_ssl );
	return( true );
#else
	return( false );

#endif /* HAVE_LIBSSL */
}

#ifdef HAVE_LIBSSL
SSL_CTX * Csock::SetupServerCTX()
{
	SSL_CTX * pCTX = GetSSLCTX( m_iMethod );
	if( !pCTX )
	{
		CS_DEBUG( "WARNING: Failed to retrieve a valid ctx" );
		return( NULL );
	}

	SSL_CTX_set_default_verify_paths( pCTX );

	// set the pemfile password
	SSL_CTX_set_default_passwd_cb( pCTX, _PemPassCB );
	SSL_CTX_set_default_passwd_cb_userdata( pCTX, ( void * )this );

	if( m_sPemFile.empty() || access( m_sPemFile.c_str(), R_OK ) != 0 )
	{
		CS_DEBUG( "Empty, missing, or bad pemfile ... [" << m_sPemFile << "]" );
		SSL_CTX_free( pCTX );
		return( NULL );
	}

	if( ! m_sKeyFile.empty() && access( m_sKeyFile.c_str(), R_OK ) != 0 )
	{
		CS_DEBUG( "Bad keyfile ... [" << m_sKeyFile << "]" );
		SSL_CTX_free( pCTX );
		return( NULL );
	}

	//
	// set up the CTX
	if( SSL_CTX_use_certificate_chain_file( pCTX, m_sPemFile.c_str() ) <= 0 )
	{
		CS_DEBUG( "Error with SSLCert file [" << m_sPemFile << "]" );
		SSLErrors( __FILE__, __LINE__ );
		SSL_CTX_free( pCTX );
		return( NULL );
	}

    CS_STRING privKeyFile = m_sKeyFile.empty() ? m_sPemFile : m_sKeyFile;
	if( SSL_CTX_use_PrivateKey_file( pCTX, privKeyFile.c_str(), SSL_FILETYPE_PEM ) <= 0 )
	{
		CS_DEBUG( "Error with SSLKey file [" << privKeyFile << "]" );
		SSLErrors( __FILE__, __LINE__ );
		SSL_CTX_free( pCTX );
		return( NULL );
	}

	// check to see if this pem file contains a DH structure for use with DH key exchange
	// https://github.com/znc/znc/pull/46
	CS_STRING DHParamFile = m_sDHParamFile.empty() ? m_sPemFile : m_sDHParamFile;
	FILE *dhParamsFile = fopen( DHParamFile.c_str(), "r" );
	if( !dhParamsFile )
	{
		CS_DEBUG( "Error with DHParam file [" << DHParamFile << "]" );
		SSL_CTX_free( pCTX );
		return( NULL );
	}

	DH * dhParams = PEM_read_DHparams( dhParamsFile, NULL, NULL, NULL );
	fclose( dhParamsFile );
	if( dhParams )
	{
		SSL_CTX_set_options( pCTX, SSL_OP_SINGLE_DH_USE );
		if( !SSL_CTX_set_tmp_dh( pCTX, dhParams ) )
		{
			CS_DEBUG( "Error setting ephemeral DH parameters from [" << m_sPemFile << "]" );
			SSLErrors( __FILE__, __LINE__ );
			DH_free( dhParams );
			SSL_CTX_free( pCTX );
			return( NULL );
		}
		DH_free( dhParams );
	}
	else
	{
		// Presumably PEM_read_DHparams failed, as there was no DH structure. Clearing those errors here so they are removed off the stack
		ERR_clear_error();
	}
#ifndef OPENSSL_NO_ECDH
	// Errors for the following block are non-fatal (ECDHE is nice to have
	// but not a requirement)
#ifndef OPENSSL_IS_BORINGSSL
	// BoringSSL does this thing automatically
#if defined( SSL_CTX_set_ecdh_auto )
	// Auto-select sensible curve
	if( !SSL_CTX_set_ecdh_auto( pCTX , 1 ) )
		ERR_clear_error();
#elif defined( SSL_CTX_set_tmp_ecdh )
	// Use a standard, widely-supported curve
	EC_KEY * ecdh = EC_KEY_new_by_curve_name( NID_X9_62_prime256v1 );
	if( ecdh )
	{
		if( !SSL_CTX_set_tmp_ecdh( pCTX, ecdh ) )
			ERR_clear_error();
		EC_KEY_free( ecdh );
	}
	else
	{
		ERR_clear_error();
	}
#endif /* SSL_CTX_set_tmp_ecdh */
#endif /* !OPENSSL_IS_BORINGSSL */
#endif /* OPENSSL_NO_ECDH */

	if( !ConfigureCTXOptions( pCTX ) )
	{
		SSL_CTX_free( pCTX );
		return( NULL );
	}
	return( pCTX );
}
#endif /* HAVE_LIBSSL */

bool Csock::SSLServerSetup()
{
#ifdef HAVE_LIBSSL
	m_bUseSSL = true;
	FREE_SSL();
	FREE_CTX();

#ifdef _WIN64
	if( m_iReadSock != ( int )m_iReadSock || m_iWriteSock != ( int )m_iWriteSock )
	{
		// sanity check the FD to be sure its compatible with openssl
		CS_DEBUG( "ERROR: sockfd larger than OpenSSL can handle" );
		return( false );
	}
#endif /* _WIN64 */

	m_ssl_ctx = SetupServerCTX();

	//
	// setup the SSL
	m_ssl = SSL_new( m_ssl_ctx );
	if( !m_ssl )
		return( false );

#if defined( SSL_MODE_SEND_FALLBACK_SCSV )
    SSL_set_mode( m_ssl, SSL_MODE_SEND_FALLBACK_SCSV );
#endif /* SSL_MODE_SEND_FALLBACK_SCSV */

	// Call for client Verification
	SSL_set_rfd( m_ssl, ( int )m_iReadSock );
	SSL_set_wfd( m_ssl, ( int )m_iWriteSock );
	SSL_set_accept_state( m_ssl );
	if( m_iRequireClientCertFlags )
	{
		SSL_set_verify( m_ssl, m_iRequireClientCertFlags, m_pCerVerifyCB );
	}
	SSL_set_info_callback( m_ssl, _InfoCallback );
	SSL_set_ex_data( m_ssl, GetCsockSSLIdx(), this );

	SSLFinishSetup( m_ssl );
	return( true );
#else
	return( false );
#endif /* HAVE_LIBSSL */
}

bool Csock::StartTLS()
{
	if( m_iConnType == INBOUND )
		return( AcceptSSL() );
	if( m_iConnType == OUTBOUND )
		return( ConnectSSL() );
	CS_DEBUG( "Invalid connection type with StartTLS" );
	return( false );
}

bool Csock::ConnectSSL()
{
#ifdef HAVE_LIBSSL
	if( m_iReadSock == CS_INVALID_SOCK )
		return( false ); // this should be long passed at this point
	if( !m_ssl && !SSLClientSetup() )
		return( false );

	bool bPass = true;

	int iErr = SSL_connect( m_ssl );
	if( iErr != 1 )
	{
		int sslErr = SSL_get_error( m_ssl, iErr );
		bPass = false;
		if( sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE )
			bPass = true;
#ifdef _WIN32
		else if( sslErr == SSL_ERROR_SYSCALL && iErr < 0 && GetLastError() == WSAENOTCONN )
		{
			// this seems to be an issue with win32 only. I've seen it happen on slow connections
			// the issue is calling this before select(), which isn't a problem on unix. Allowing this
			// to pass in this case is fine because subsequent ssl transactions will occur and the handshake
			// will finish. At this point, its just instantiating the handshake.
			bPass = true;
		}
#endif /* _WIN32 */
	}
	else
	{
		bPass = true;
	}

	if( m_eConState != CST_OK )
		m_eConState = CST_OK;
	return( bPass );
#else
	return( false );
#endif /* HAVE_LIBSSL */
}

#ifdef HAVE_ICU
inline bool icuConv( const CS_STRING& src, CS_STRING& tgt, UConverter* cnv_in, UConverter* cnv_out )
{
	const char* indata = src.c_str();
	const char* indataend = indata + src.length();
	tgt.clear();
	char buf[100];
	UChar pivotStart[100];
	UChar* pivotSource = pivotStart;
	UChar* pivotTarget = pivotStart;
	UChar* pivotLimit = pivotStart + sizeof pivotStart / sizeof pivotStart[0];
	const char* outdataend = buf + sizeof buf;
	bool reset = true;
	while( true )
	{
		char* outdata = buf;
		UErrorCode e = U_ZERO_ERROR;
		ucnv_convertEx( cnv_out, cnv_in, &outdata, outdataend, &indata, indataend, pivotStart, &pivotSource, &pivotTarget, pivotLimit, reset, true, &e );
		reset = false;
		if( U_SUCCESS( e ) )
		{
			if( e != U_ZERO_ERROR )
			{
				CS_DEBUG( "Warning during converting string encoding: " << u_errorName( e ) );
			}
			tgt.append( buf, outdata - buf );
			break;
		}
		if( e == U_BUFFER_OVERFLOW_ERROR )
		{
			tgt.append( buf, outdata - buf );
			continue;
		}
		CS_DEBUG( "Error during converting string encoding: " << u_errorName( e ) );
		return false;
	}
	return true;
}

static bool isUTF8( const CS_STRING& src, CS_STRING& target)
{
	UErrorCode e = U_ZERO_ERROR;
	// Convert to UTF-16 without actually converting. This will still count
	// the number of output characters, so we either get
	// U_BUFFER_OVERFLOW_ERROR or U_INVALID_CHAR_FOUND.
	u_strFromUTF8( NULL, 0, NULL, src.c_str(), (int32_t) src.length(), &e );
	if( e != U_BUFFER_OVERFLOW_ERROR)
		return false;
	target = src;
	return true;
}
#endif /* HAVE_ICU */

bool Csock::AllowWrite( uint64_t & iNOW ) const
{
	if( m_iMaxBytes > 0 && m_iMaxMilliSeconds > 0 )
	{
		if( iNOW == 0 )
			iNOW = millitime();

		if( m_iLastSend <  m_iMaxBytes )
			return( true ); // allow sending if our out buffer was less than what we can send
		if( ( iNOW - m_iLastSendTime ) < m_iMaxMilliSeconds )
			return( false );
	}
	return( true );
}

void Csock::ShrinkSendBuff()
{
	if( m_uSendBufferPos > 0 )
	{
		// just doing this to keep m_sSend from growing out of control
		m_sSend.erase( 0, m_uSendBufferPos );
		m_uSendBufferPos = 0;
	}
}
void Csock::IncBuffPos( size_t uBytes )
{
	m_uSendBufferPos += uBytes;
	if( m_uSendBufferPos >= m_sSend.size() )
	{
		m_uSendBufferPos = 0;
		m_sSend.clear();
	}
}

bool Csock::Write( const char *data, size_t len )
{
	if( len > 0 )
	{
		ShrinkSendBuff();
		m_sSend.append( data, len );
	}

	if( m_sSend.empty() )
		return( true );

	if( m_eConState != CST_OK )
		return( true );

	// rate shaping
	size_t iBytesToSend = 0;

	size_t uBytesInSend = m_sSend.size() - m_uSendBufferPos;

#ifdef HAVE_LIBSSL
	if( m_bUseSSL && m_sSSLBuffer.empty() && !m_bsslEstablished )
	{
		// to keep openssl from spinning, just initiate the connection with 1 byte so the connection establishes faster
		iBytesToSend = 1;
	}
	else
#endif /* HAVE_LIBSSL */
		if( m_iMaxBytes > 0 && m_iMaxMilliSeconds > 0 )
		{
			uint64_t iNOW = millitime();
			// figure out the shaping here
			// if NOW - m_iLastSendTime > m_iMaxMilliSeconds then send a full length of ( iBytesToSend )
			if( ( iNOW - m_iLastSendTime ) > m_iMaxMilliSeconds )
			{
				m_iLastSendTime = iNOW;
				iBytesToSend = m_iMaxBytes;
				m_iLastSend = 0;
			}
			else   // otherwise send m_iMaxBytes - m_iLastSend
				iBytesToSend = m_iMaxBytes - m_iLastSend;

			// take which ever is lesser
			if( uBytesInSend < iBytesToSend )
				iBytesToSend = uBytesInSend;

			// add up the bytes sent
			m_iLastSend += iBytesToSend;

			// so, are we ready to send anything ?
			if( iBytesToSend == 0 )
				return( true );
		}
		else
		{
			iBytesToSend = uBytesInSend;
		}

#ifdef HAVE_LIBSSL
	if( m_bUseSSL )
	{
		if( !m_ssl )
		{
			CS_DEBUG( "SSL object is NULL but m_bUseSSL is true" );
			return( false );
		}

		if( m_sSSLBuffer.empty() )  // on retrying to write data, ssl wants the data in the SAME spot and the SAME size
			m_sSSLBuffer.append( m_sSend.data() + m_uSendBufferPos, iBytesToSend );

		int iErr = SSL_write( m_ssl, m_sSSLBuffer.data(), ( int )m_sSSLBuffer.length() );

		if( iErr < 0 && GetSockError() == ECONNREFUSED )
		{
			// If ret == -1, the underlying BIO reported an I/O error (man SSL_get_error)
			ConnectionRefused();
			return( false );
		}

		switch( SSL_get_error( m_ssl, iErr ) )
		{
			case SSL_ERROR_NONE:
				m_bsslEstablished = true;
				// all ok
				break;

			case SSL_ERROR_ZERO_RETURN:
			{
				// weird closer alert
				return( false );
			}

			case SSL_ERROR_WANT_READ:
				// retry
				break;

			case SSL_ERROR_WANT_WRITE:
				// retry
				break;

			case SSL_ERROR_SSL:
			{
				SSLErrors( __FILE__, __LINE__ );
				return( false );
			}
		}

		if( iErr > 0 )
		{
			m_sSSLBuffer.clear();
			IncBuffPos( ( size_t )iErr );
			// reset the timer on successful write (we have to set it here because the write
			// bit might not always be set, so need to trigger)
			if( TMO_WRITE & GetTimeoutType() )
				ResetTimer();

			m_iBytesWritten += ( uint64_t )iErr;
		}

		return( true );
	}
#endif /* HAVE_LIBSSL */
#ifdef _WIN32
	cs_ssize_t bytes = send( m_iWriteSock, m_sSend.data() + m_uSendBufferPos, iBytesToSend, 0 );
#else
	cs_ssize_t bytes = write( m_iWriteSock, m_sSend.data() + m_uSendBufferPos, iBytesToSend );
#endif /* _WIN32 */

	if( bytes == -1 && GetSockError() == ECONNREFUSED )
	{
		ConnectionRefused();
		return( false );
	}

#ifdef _WIN32
	if( bytes <= 0 && GetSockError() != WSAEWOULDBLOCK )
		return( false );
#else
	if( bytes <= 0 && GetSockError() != EAGAIN )
		return( false );
#endif /* _WIN32 */

	// delete the bytes we sent
	if( bytes > 0 )
	{
		IncBuffPos( ( size_t )bytes );
		if( TMO_WRITE & GetTimeoutType() )
			ResetTimer();	// reset the timer on successful write
		m_iBytesWritten += ( uint64_t )bytes;
	}

	return( true );
}

bool Csock::Write( const CS_STRING & sData )
{
#ifdef HAVE_ICU
	if( m_cnvExt && !m_cnvSendUTF8 )
	{
		CS_STRING sBinary;
		if( icuConv( sData, sBinary, m_cnvInt, m_cnvExt ) )
		{
			return( Write( sBinary.c_str(), sBinary.length() ) );
		}
	}
	// can't convert our UTF-8 string to that encoding, just put it as is...
#endif /* HAVE_ICU */
	return( Write( sData.c_str(), sData.length() ) );
}

cs_ssize_t Csock::Read( char *data, size_t len )
{
	cs_ssize_t bytes = 0;

	if( IsReadPaused() && SslIsEstablished() )
		return( READ_EAGAIN ); // allow the handshake to complete first

#ifdef HAVE_LIBSSL
	if( m_bUseSSL && m_eConState != CST_CONNECTWAIT )
	{
		if( !m_ssl )
		{
			CS_DEBUG( "SSL object is NULL but m_bUseSSL is true" );
			return( READ_ERR );
		}

		bytes = SSL_read( m_ssl, data, ( int )len );
		if( bytes >= 0 )
			m_bsslEstablished = true; // this means all is good in the realm of ssl
	}
	else
#endif /* HAVE_LIBSSL */
#ifdef _WIN32
		bytes = recv( m_iReadSock, data, len, 0 );
#else
		bytes = read( m_iReadSock, data, len );
#endif /* _WIN32 */
	if( bytes == -1 )
	{
		if( GetSockError() == ECONNREFUSED )
			return( READ_CONNREFUSED );

		if( GetSockError() == ETIMEDOUT )
			return( READ_TIMEDOUT );

		if( GetSockError() == EINTR || GetSockError() == EAGAIN )
			return( READ_EAGAIN );

#ifdef _WIN32
		if( GetSockError() == WSAEWOULDBLOCK )
			return( READ_EAGAIN );
#endif /* _WIN32 */

#ifdef HAVE_LIBSSL
		if( m_ssl )
		{
			int iErr = SSL_get_error( m_ssl, ( int )bytes );
			if( iErr != SSL_ERROR_WANT_READ && iErr != SSL_ERROR_WANT_WRITE )
				return( READ_ERR );
			else
				return( READ_EAGAIN );
		}
#else
		return( READ_ERR );
#endif /* HAVE_LIBSSL */
	}

	if( bytes > 0 ) // becareful not to add negative bytes :P
		m_iBytesRead += ( uint64_t )bytes;

	return( bytes );
}

CS_STRING Csock::GetLocalIP() const
{
	if( !m_sLocalIP.empty() )
		return( m_sLocalIP );

	cs_sock_t iSock = GetSock();

	if( iSock == CS_INVALID_SOCK )
		return( "" );

	struct sockaddr_storage cAddr;
	socklen_t iAddrLen = sizeof( cAddr );
	if( getsockname( iSock, ( struct sockaddr * )&cAddr, &iAddrLen ) == 0 )
	{
		ConvertAddress( &cAddr, iAddrLen, m_sLocalIP, &m_iLocalPort );
	}

	return( m_sLocalIP );
}

CS_STRING Csock::GetRemoteIP() const
{
	if( !m_sRemoteIP.empty() )
		return( m_sRemoteIP );

	cs_sock_t iSock = GetSock();

	if( iSock == CS_INVALID_SOCK )
		return( "" );

	struct sockaddr_storage cAddr;
	socklen_t iAddrLen = sizeof( cAddr );
	if( getpeername( iSock, ( struct sockaddr * )&cAddr, &iAddrLen ) == 0 )
	{
		ConvertAddress( &cAddr, iAddrLen, m_sRemoteIP, &m_iRemotePort );
	}

	return( m_sRemoteIP );
}

bool Csock::IsConnected() const { return( m_bIsConnected ); }
void Csock::SetIsConnected( bool b )
{
	m_bIsConnected = b;
	if( m_eConState == CST_CONNECTWAIT && b )
		m_eConState = ( GetSSL() ? CST_CONNECTSSL : CST_OK );
}

cs_sock_t & Csock::GetRSock() { return( m_iReadSock ); }
const cs_sock_t & Csock::GetRSock() const { return( m_iReadSock ); }
void Csock::SetRSock( cs_sock_t iSock ) { m_iReadSock = iSock; }
cs_sock_t & Csock::GetWSock() { return( m_iWriteSock ); }
const cs_sock_t & Csock::GetWSock() const { return( m_iWriteSock ); }
void Csock::SetWSock( cs_sock_t iSock ) { m_iWriteSock = iSock; }
void Csock::SetSock( cs_sock_t iSock ) { m_iWriteSock = iSock; m_iReadSock = iSock; }
cs_sock_t & Csock::GetSock() { return( m_iReadSock ); }
const cs_sock_t & Csock::GetSock() const { return( m_iReadSock ); }
void Csock::ResetTimer() { m_iLastCheckTimeoutTime = 0; m_iTcount = 0; }
void Csock::PauseRead() { m_bPauseRead = true; }
bool Csock::IsReadPaused() const { return( m_bPauseRead ); }

void Csock::UnPauseRead()
{
	m_bPauseRead = false;
	ResetTimer();
	PushBuff( "", 0, true );
}

void Csock::SetTimeout( int iTimeout, u_int iTimeoutType )
{
	m_iTimeoutType = iTimeoutType;
	m_iTimeout = iTimeout;
}

void Csock::CallSockError( int iErrno, const CS_STRING & sDescription )
{
	if( !sDescription.empty() )
	{
		SockError( iErrno, sDescription );
	}
	else
	{
		char szBuff[0xff];
		SockError( iErrno, CS_StrError( iErrno, szBuff, 0xff ) );
	}
}
void Csock::SetTimeoutType( u_int iTimeoutType ) { m_iTimeoutType = iTimeoutType; }
int Csock::GetTimeout() const { return m_iTimeout; }
u_int Csock::GetTimeoutType() const { return( m_iTimeoutType ); }

bool Csock::CheckTimeout( time_t iNow )
{
	if( m_iLastCheckTimeoutTime == 0 )
	{
		m_iLastCheckTimeoutTime = iNow;
		return( false );
	}

	if( IsReadPaused() )
		return( false );

	time_t iDiff = 0;
	if( iNow > m_iLastCheckTimeoutTime )
	{
		iDiff = iNow - m_iLastCheckTimeoutTime;
	}
	else
	{
		// this is weird, but its possible if someone changes a clock and it went back in time, this essentially has to reset the last check
		// the worst case scenario is the timeout is about to it and the clock changes, it would then cause
		// this to pass over the last half the time
		m_iLastCheckTimeoutTime = iNow;
	}

	if( m_iTimeout > 0 )
	{
		// this is basically to help stop a clock adjust ahead, stuff could reset immediatly on a clock jump
		// otherwise
		time_t iRealTimeout = m_iTimeout;
		if( iRealTimeout <= 1 )
			m_iTcount++;
		else if( m_iTcount == 0 )
			iRealTimeout /= 2;
		if( iDiff >= iRealTimeout )
		{
			if( m_iTcount == 0 )
				m_iLastCheckTimeoutTime = iNow - iRealTimeout;
			if( m_iTcount++ >= 1 )
			{
				Timeout();
				return( true );
			}
		}
	}
	return( false );
}

void Csock::PushBuff( const char *data, size_t len, bool bStartAtZero )
{
	if( !m_bEnableReadLine )
		return;	// If the ReadLine event is disabled, just ditch here

	size_t iStartPos = ( m_sbuffer.empty() || bStartAtZero ? 0 : m_sbuffer.length() - 1 );

	if( data )
		m_sbuffer.append( data, len );

	while( !m_bPauseRead && GetCloseType() == CLT_DONT )
	{
		CS_STRING::size_type iFind = m_sbuffer.find( "\n", iStartPos );

		if( iFind != CS_STRING::npos )
		{
			CS_STRING sBuff = m_sbuffer.substr( 0, iFind + 1 );	// read up to(including) the newline
			m_sbuffer.erase( 0, iFind + 1 );					// erase past the newline
#ifdef HAVE_ICU
			if( m_cnvExt )
			{
				CS_STRING sUTF8;
				if( ( m_cnvTryUTF8 && isUTF8( sBuff, sUTF8 ) ) // maybe it's already UTF-8?
				        || icuConv( sBuff, sUTF8, m_cnvExt, m_cnvInt ) )
				{
					ReadLine( sUTF8 );
				}
				else
				{
					CS_DEBUG( "Can't convert received line to UTF-8" );
				}
			}
			else
#endif /* HAVE_ICU */
			{
				ReadLine( sBuff );
			}
			iStartPos = 0; // reset this back to 0, since we need to look for the next newline here.
		}
		else
		{
			break;
		}
	}

	if( m_iMaxStoredBufferLength > 0 && m_sbuffer.length() > m_iMaxStoredBufferLength )
		ReachedMaxBuffer(); // call the max read buffer event
}

#ifdef HAVE_ICU
void Csock::IcuExtToUCallback(
		UConverterToUnicodeArgs* toArgs,
		const char* codeUnits,
		int32_t length,
		UConverterCallbackReason reason,
		UErrorCode* err)
{
	if( reason <= UCNV_IRREGULAR )
	{
		*err = U_ZERO_ERROR;
		ucnv_cbToUWriteSub( toArgs, 0, err );
	}
}

void Csock::IcuExtFromUCallback(
		UConverterFromUnicodeArgs* fromArgs,
		const UChar* codeUnits,
		int32_t length,
		UChar32 codePoint,
		UConverterCallbackReason reason,
		UErrorCode* err)
{
	if( reason <= UCNV_IRREGULAR )
	{
		*err = U_ZERO_ERROR;
		ucnv_cbFromUWriteSub( fromArgs, 0, err );
	}
}

static void icuExtToUCallback(
		const void* context,
		UConverterToUnicodeArgs* toArgs,
		const char* codeUnits,
		int32_t length,
		UConverterCallbackReason reason,
		UErrorCode* err)
{
	Csock* pcSock = (Csock*)context;
	pcSock->IcuExtToUCallback(toArgs, codeUnits, length, reason, err);
}

static void icuExtFromUCallback(
		const void* context,
		UConverterFromUnicodeArgs* fromArgs,
		const UChar* codeUnits,
		int32_t length,
		UChar32 codePoint,
		UConverterCallbackReason reason,
		UErrorCode* err)
{
	Csock* pcSock = (Csock*)context;
	pcSock->IcuExtFromUCallback(fromArgs, codeUnits, length, codePoint, reason, err);
}

void Csock::SetEncoding( const CS_STRING& sEncoding )
{
	if( m_cnvExt ) ucnv_close( m_cnvExt );
	m_cnvExt = NULL;
	m_sEncoding = sEncoding;
	if( !sEncoding.empty() )
	{
		m_cnvTryUTF8 = sEncoding[0] == '*' || sEncoding[0] == '^';
		m_cnvSendUTF8 = sEncoding[0] == '^';
		const char* sEncodingName = sEncoding.c_str();
		if( m_cnvTryUTF8 )
			sEncodingName++;
		UErrorCode e = U_ZERO_ERROR;
		m_cnvExt = ucnv_open( sEncodingName, &e );
		if( U_FAILURE( e ) )
		{
			CS_DEBUG( "Can't set encoding to " << sEncoding << ": " <<  u_errorName( e ) );
		}
		if( m_cnvExt )
		{
			ucnv_setToUCallBack( m_cnvExt, icuExtToUCallback, this, NULL, NULL, &e );
			ucnv_setFromUCallBack( m_cnvExt, icuExtFromUCallback, this, NULL, NULL, &e );
		}
	}
}
#endif /* HAVE_ICU */

CS_STRING & Csock::GetInternalReadBuffer() { return( m_sbuffer ); }
CS_STRING & Csock::GetInternalWriteBuffer()
{
	// in the event that some is grabbing this for some reason, we need to
	// clean it up so it's what they are expecting.
	ShrinkSendBuff();
	return( m_sSend );
}
void Csock::SetMaxBufferThreshold( u_int iThreshold ) { m_iMaxStoredBufferLength = iThreshold; }
u_int Csock::GetMaxBufferThreshold() const { return( m_iMaxStoredBufferLength ); }
int Csock::GetType() const { return( m_iConnType ); }
void Csock::SetType( int iType ) { m_iConnType = iType; }
const CS_STRING & Csock::GetSockName() const { return( m_sSockName ); }
void Csock::SetSockName( const CS_STRING & sName ) { m_sSockName = sName; }
const CS_STRING & Csock::GetHostName() const { return( m_shostname ); }
void Csock::SetHostName( const CS_STRING & sHostname ) { m_shostname = sHostname; }
uint64_t Csock::GetStartTime() const { return( m_iStartTime ); }
void Csock::ResetStartTime() { m_iStartTime = 0; }
uint64_t Csock::GetBytesRead() const { return( m_iBytesRead ); }
void Csock::ResetBytesRead() { m_iBytesRead = 0; }
uint64_t Csock::GetBytesWritten() const { return( m_iBytesWritten ); }
void Csock::ResetBytesWritten() { m_iBytesWritten = 0; }

double Csock::GetAvgRead( uint64_t iSample ) const
{
	uint64_t iDifference = ( millitime() - m_iStartTime );

	if( m_iBytesRead == 0 || iSample > iDifference )
		return( ( double )m_iBytesRead );

	return( ( ( double )m_iBytesRead / ( ( double )iDifference / ( double )iSample ) ) );
}

double Csock::GetAvgWrite( uint64_t iSample ) const
{
	uint64_t iDifference = ( millitime() - m_iStartTime );

	if( m_iBytesWritten == 0 || iSample > iDifference )
		return( ( double )m_iBytesWritten );

	return( ( ( double )m_iBytesWritten / ( ( double )iDifference / ( double )iSample ) ) );
}

uint16_t Csock::GetRemotePort() const
{
	if( m_iRemotePort > 0 )
		return( m_iRemotePort );
	GetRemoteIP();
	return( m_iRemotePort );
}

uint16_t Csock::GetLocalPort() const
{
	if( m_iLocalPort > 0 )
		return( m_iLocalPort );
	GetLocalIP();
	return( m_iLocalPort );
}

uint16_t Csock::GetPort() const { return( m_uPort ); }
void Csock::SetPort( uint16_t iPort ) { m_uPort = iPort; }
void Csock::Close( ECloseType eCloseType )
{
	m_eCloseType = eCloseType;
}

void Csock::NonBlockingIO()
{
	set_non_blocking( m_iReadSock );

	if( m_iReadSock != m_iWriteSock )
	{
		set_non_blocking( m_iWriteSock );
	}
}

bool Csock::GetSSL() const { return( m_bUseSSL ); }
void Csock::SetSSL( bool b ) { m_bUseSSL = b; }

#ifdef HAVE_LIBSSL
void Csock::SetCipher( const CS_STRING & sCipher ) { m_sCipherType = sCipher; }
const CS_STRING & Csock::GetCipher() const { return( m_sCipherType ); }

void Csock::SetDHParamLocation( const CS_STRING & sDHParamFile ) { m_sDHParamFile = sDHParamFile; }
const CS_STRING & Csock::GetDHParamLocation() const { return( m_sDHParamFile ); }

void Csock::SetKeyLocation( const CS_STRING & sKeyFile ) { m_sKeyFile = sKeyFile; }
const CS_STRING & Csock::GetKeyLocation() const { return( m_sKeyFile ); }

void Csock::SetPemLocation( const CS_STRING & sPemFile ) { m_sPemFile = sPemFile; }
const CS_STRING & Csock::GetPemLocation() const { return( m_sPemFile ); }

void Csock::SetPemPass( const CS_STRING & sPassword ) { m_sPemPass = sPassword; }
const CS_STRING & Csock::GetPemPass() const { return( m_sPemPass ); }

void Csock::SetSSLMethod( int iMethod ) { m_iMethod = iMethod; }
int Csock::GetSSLMethod() const { return( m_iMethod ); }
void Csock::SetSSLObject( SSL *ssl, bool bDeleteExisting ) 
{ 
	if( bDeleteExisting )
		FREE_SSL();
	m_ssl = ssl; 
}
SSL * Csock::GetSSLObject() const
{
	if( m_ssl )
		return( m_ssl );

	return( NULL );
}
void Csock::SetCTXObject( SSL_CTX *sslCtx, bool bDeleteExisting ) 
{
	if( bDeleteExisting )
		FREE_CTX();
	m_ssl_ctx = sslCtx; 
}

SSL_SESSION * Csock::GetSSLSession() const
{
	if( m_ssl )
		return( SSL_get_session( m_ssl ) );

	return( NULL );
}
#endif /* HAVE_LIBSSL */

bool Csock::HasWriteBuffer() const
{
	// the fact that this has data in it is good enough. Checking m_uSendBufferPos is a moot point
	// since once m_uSendBufferPos is at the same position as m_sSend it's cleared (in Write)
	return( !m_sSend.empty() );
}
void Csock::ClearWriteBuffer() { m_sSend.clear(); m_uSendBufferPos = 0; }
bool Csock::SslIsEstablished() const { return ( m_bsslEstablished ); }

bool Csock::ConnectInetd( bool bIsSSL, const CS_STRING & sHostname )
{
	if( !sHostname.empty() )
		m_sSockName = sHostname;

	// set our hostname
	if( m_sSockName.empty() )
	{
		struct sockaddr_in client;
		socklen_t clen = sizeof( client );
		if( getpeername( 0, ( struct sockaddr * )&client, &clen ) < 0 )
		{
			m_sSockName = "0.0.0.0:0";
		}
		else
		{
			stringstream s;
			s << inet_ntoa( client.sin_addr ) << ":" << ntohs( client.sin_port );
			m_sSockName = s.str();
		}
	}

	return( ConnectFD( 0, 1, m_sSockName, bIsSSL, INBOUND ) );
}

bool Csock::ConnectFD( int iReadFD, int iWriteFD, const CS_STRING & sName, bool bIsSSL, ETConn eDirection )
{
	if( eDirection == LISTENER )
	{
		CS_DEBUG( "You can not use a LISTENER type here!" );
		return( false );
	}

	// set our socket type
	SetType( eDirection );

	// set the hostname
	m_sSockName = sName;

	// set the file descriptors
	SetRSock( iReadFD );
	SetWSock( iWriteFD );

	// set it up as non-blocking io
	NonBlockingIO();

	if( bIsSSL )
	{
		if( eDirection == INBOUND && !AcceptSSL() )
			return( false );
		else if( eDirection == OUTBOUND && !ConnectSSL() )
			return( false );
	}

	return( true );
}

#ifdef HAVE_LIBSSL
X509 *Csock::GetX509() const
{
	if( m_ssl )
		return( SSL_get_peer_certificate( m_ssl ) );

	return( NULL );
}

CS_STRING Csock::GetPeerPubKey() const
{
	CS_STRING sKey;

	X509 * pCert = GetX509();

	if( pCert )
	{
		EVP_PKEY * pKey = X509_get_pubkey( pCert );
		if( pKey )
		{
			const BIGNUM * pPubKey = NULL;
#ifdef HAVE_OPAQUE_SSL
			int iType = EVP_PKEY_base_id( pKey );
#else
			int iType = pKey->type;
#endif /* HAVE_OPAQUE_SSL */
			switch( iType )
			{
#ifndef OPENSSL_NO_RSA
			case EVP_PKEY_RSA:
# ifdef HAVE_OPAQUE_SSL
				RSA_get0_key( EVP_PKEY_get0_RSA( pKey ), &pPubKey, NULL, NULL );
# else
				pPubKey = pKey->pkey.rsa->n;
# endif /* HAVE_OPAQUE_SSL */
				break;
#endif /* OPENSSL_NO_RSA */
#ifndef OPENSSL_NO_DSA
			case EVP_PKEY_DSA:
# ifdef HAVE_OPAQUE_SSL
				DSA_get0_key( EVP_PKEY_get0_DSA( pKey ), &pPubKey, NULL );
# else
				pPubKey = pKey->pkey.dsa->pub_key;
# endif /* HAVE_OPAQUE_SSL */
				break;
#endif /* OPENSSL_NO_DSA */
			default:
				CS_DEBUG( "Not Prepared for Public Key Type [" << iType << "]" );
				break;
			}
			if( pPubKey )
			{
				char *hxKey = BN_bn2hex( pPubKey );
				sKey = hxKey;
				OPENSSL_free( hxKey );
			}
			EVP_PKEY_free( pKey );
		}
		X509_free( pCert );
	}
	return( sKey );
}

long Csock::GetPeerFingerprint( CS_STRING & sFP ) const
{
	sFP.clear();

	if( !m_ssl )
		return( 0 );

	X509 * pCert = GetX509();

#ifdef HAVE_OPAQUE_SSL
	unsigned char sha1_hash[SHA_DIGEST_LENGTH];

	if( pCert && X509_digest( pCert, EVP_sha1(), sha1_hash, NULL ) )
#else
	unsigned char * sha1_hash = NULL;

	// Inspired by charybdis
	if( pCert && (sha1_hash = pCert->sha1_hash) )
#endif /* HAVE_OPAQUE_SSL */
	{
		for( int i = 0; i < SHA_DIGEST_LENGTH; i++ )
		{
			char buf[3];
			snprintf( buf, 3, "%02x", sha1_hash[i] );
			sFP += buf;
		}
	}
	X509_free( pCert );

	return( SSL_get_verify_result( m_ssl ) );
}
u_int Csock::GetRequireClientCertFlags() const { return( m_iRequireClientCertFlags ); }
void Csock::SetRequiresClientCert( bool bRequiresCert ) { m_iRequireClientCertFlags = ( bRequiresCert ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT|SSL_VERIFY_PEER : 0 ); }

#endif /* HAVE_LIBSSL */

void Csock::SetParentSockName( const CS_STRING & sParentName ) { m_sParentName = sParentName; }
const CS_STRING & Csock::GetParentSockName() const { return( m_sParentName ); }

void Csock::SetRate( u_int iBytes, uint64_t iMilliseconds )
{
	m_iMaxBytes = iBytes;
	m_iMaxMilliSeconds = iMilliseconds;
}

u_int Csock::GetRateBytes() const { return( m_iMaxBytes ); }
uint64_t Csock::GetRateTime() const { return( m_iMaxMilliSeconds ); }


void Csock::EnableReadLine() { m_bEnableReadLine = true; }
void Csock::DisableReadLine()
{
	m_bEnableReadLine = false;
	m_sbuffer.clear();
}

void Csock::ReachedMaxBuffer()
{
	std::cerr << "Warning, Max Buffer length Warning Threshold has been hit" << endl;
	std::cerr << "If you don't care, then set SetMaxBufferThreshold to 0" << endl;
}

time_t Csock::GetTimeSinceLastDataTransaction( time_t iNow ) const
{
	if( m_iLastCheckTimeoutTime == 0 )
		return( 0 );
	return( ( iNow > 0 ? iNow : time( NULL ) ) - m_iLastCheckTimeoutTime );
}

time_t Csock::GetNextCheckTimeout( time_t iNow ) const
{
	if( iNow == 0 )
		iNow = time( NULL );
	time_t iTimeout = m_iTimeout;
	time_t iDiff = iNow - m_iLastCheckTimeoutTime;
	/* CheckTimeout() wants to be called after half the timeout */
	if( m_iTcount == 0 )
		iTimeout /= 2;
	if( iDiff > iTimeout )
		iTimeout = 0;
	else
		iTimeout -= iDiff;
	return( iNow + iTimeout );
}

int Csock::GetPending() const
{
#ifdef HAVE_LIBSSL
	if( m_ssl )
	{
		// in v23 method, the pending function is initialized to ssl_undefined_const_function
		// which throws SSL_UNDEFINED_CONST_FUNCTION on to the error stack
		// this is one of the more stupid things in openssl, it seems bizarre that even though SSL_pending
		// returns an int, they don't bother returning in error to notify us, so basically
		// we have to always clear errors here generated by SSL_pending, otherwise the stack could
		// have a lame error on it causing SSL_write to fail in certain instances.
#if defined( OPENSSL_VERSION_NUMBER ) && OPENSSL_VERSION_NUMBER >= 0x00908000
		ERR_set_mark();
		int iBytes = SSL_pending( m_ssl );
		ERR_pop_to_mark();
		return( iBytes );
#else
		int iBytes = SSL_pending( m_ssl );
		ERR_clear_error(); // to get safer handling, upgrade your openssl version!
		return( iBytes );
#endif /* OPENSSL_VERSION_NUMBER */
	}
	else
		return( 0 );
#else
	return( 0 );
#endif /* HAVE_LIBSSL */
}

bool Csock::CreateSocksFD()
{
	if( m_iReadSock != CS_INVALID_SOCK )
		return( true );

	m_iReadSock = m_iWriteSock = CreateSocket();
	if( m_iReadSock == CS_INVALID_SOCK )
		return( false );

	m_address.SinFamily();
	m_address.SinPort( m_uPort );

	return( true );
}


int Csock::GetAddrInfo( const CS_STRING & sHostname, CSSockAddr & csSockAddr )
{
#ifdef HAVE_IPV6
	if( csSockAddr.GetAFRequire() != AF_INET && inet_pton( AF_INET6, sHostname.c_str(), csSockAddr.GetAddr6() ) > 0 )
	{
		SetIPv6( true );
		return( 0 );
	}
#endif /* HAVE_IPV6 */
	if( inet_pton( AF_INET, sHostname.c_str(), csSockAddr.GetAddr() ) > 0 )
	{
#ifdef HAVE_IPV6
		SetIPv6( false );
#endif /* HAVE_IPV6 */
		return( 0 );
	}

#ifdef HAVE_C_ARES
	// need to compute this up here
	if( !m_pARESChannel )
	{
		if( ares_init( &m_pARESChannel ) != ARES_SUCCESS )
		{
			// TODO throw some debug?
			FreeAres();
			return( ETIMEDOUT );
		}
		m_pCurrAddr = &csSockAddr; // flag its starting

		int iFamily = AF_INET;
#ifdef HAVE_IPV6
#if ARES_VERSION >= CREATE_ARES_VER( 1, 7, 5 )
		// as of ares 1.7.5, it falls back to af_inet only when AF_UNSPEC is specified
		// so this can finally let the code flow through as anticipated :)
		iFamily = csSockAddr.GetAFRequire();
#else
		// as of ares 1.6.0 if it fails on af_inet6, it falls back to af_inet,
		// this code was here in the previous Csocket version, just adding the comment as a reminder
		iFamily = csSockAddr.GetAFRequire() == CSSockAddr::RAF_ANY ? AF_INET6 : csSockAddr.GetAFRequire();
#endif /* CREATE_ARES_VER( 1, 7, 5 ) */
#endif /* HAVE_IPV6 */
		ares_gethostbyname( m_pARESChannel, sHostname.c_str(), iFamily, AresHostCallback, this );
	}
	if( !m_pCurrAddr )
	{
		// this means its finished
		FreeAres();
#ifdef HAVE_IPV6
		if( GetType() != LISTENER && m_iARESStatus == ARES_SUCCESS && csSockAddr.GetAFRequire() == CSSockAddr::RAF_ANY && GetIPv6() )
		{
			// this means that ares_host returned an ipv6 host, so try a connect right away
			if( CreateSocksFD() && Connect() )
			{
				SetSkipConnect( true );
			}
#ifndef _WIN32
			else if( GetSockError() == ENETUNREACH )
#else
			else if( GetSockError() == WSAENETUNREACH || GetSockError() == WSAEHOSTUNREACH )
#endif /* !_WIN32 */
			{
				// the Connect() failed, so throw a retry back in with ipv4, and let it process normally
				CS_DEBUG( "Failed ipv6 connection with PF_UNSPEC, falling back to ipv4" );
				m_iARESStatus = -1;
				CloseSocksFD();
				SetAFRequire( CSSockAddr::RAF_INET );
				return( GetAddrInfo( sHostname, csSockAddr ) );
			}
		}
#if ARES_VERSION < CREATE_ARES_VER( 1, 5, 3 )
		if( m_iARESStatus != ARES_SUCCESS && csSockAddr.GetAFRequire() == CSSockAddr::RAF_ANY )
		{
			// this is a workaround for ares < 1.5.3 where the builtin retry on failed AF_INET6 isn't there yet
			CS_DEBUG( "Retry for older version of c-ares with AF_INET only" );
			// this means we tried previously with AF_INET6 and failed, so force AF_INET and retry
			SetAFRequire( CSSockAddr::RAF_INET );
			return( GetAddrInfo( sHostname, csSockAddr ) );
		}
#endif /* ARES_VERSION < CREATE_ARES_VER( 1, 5, 3 ) */
#endif /* HAVE_IPV6 */
		return( m_iARESStatus == ARES_SUCCESS ? 0 : ETIMEDOUT );
	}
	return( EAGAIN );
#else /* HAVE_C_ARES */
	return( ::CS_GetAddrInfo( sHostname, this, csSockAddr ) );
#endif /* HAVE_C_ARES */
}

int Csock::DNSLookup( EDNSLType eDNSLType )
{
	if( eDNSLType == DNS_VHOST )
	{
		if( m_sBindHost.empty() )
		{
			if( m_eConState != CST_OK )
				m_eConState = CST_DESTDNS; // skip binding, there is no vhost
			return( 0 );
		}

		m_bindhost.SinFamily();
		m_bindhost.SinPort( 0 );
	}

	int iRet = ETIMEDOUT;
	if( eDNSLType == DNS_VHOST )
	{
		iRet = GetAddrInfo( m_sBindHost, m_bindhost );
#ifdef HAVE_IPV6
		if( m_bindhost.GetIPv6() )
		{
			SetAFRequire( CSSockAddr::RAF_INET6 );
		}
		else
		{
			SetAFRequire( CSSockAddr::RAF_INET );
		}
#endif /* HAVE_IPV6 */
	}
	else
	{
		iRet = GetAddrInfo( m_shostname, m_address );
	}

	if( iRet == 0 )
	{
		if( !CreateSocksFD() )
		{
			m_iDNSTryCount = 0;
			return( ETIMEDOUT );
		}
		if( m_eConState != CST_OK )
			m_eConState = ( ( eDNSLType == DNS_VHOST ) ? CST_BINDVHOST : CST_CONNECT );
		m_iDNSTryCount = 0;
		return( 0 );
	}
	else if( iRet == EAGAIN )
	{
#ifndef HAVE_C_ARES
		m_iDNSTryCount++;
		if( m_iDNSTryCount > 20 )
		{
			m_iDNSTryCount = 0;
			return( ETIMEDOUT );
		}
#endif /* HAVE_C_ARES */
		return( EAGAIN );
	}
	m_iDNSTryCount = 0;
	return( ETIMEDOUT );
}

bool Csock::SetupVHost()
{
	if( m_sBindHost.empty() )
	{
		if( m_eConState != CST_OK )
			m_eConState = CST_DESTDNS;
		return( true );
	}
	int iRet = -1;
	if( !GetIPv6() )
		iRet = bind( m_iReadSock, ( struct sockaddr * ) m_bindhost.GetSockAddr(), m_bindhost.GetSockAddrLen() );
#ifdef HAVE_IPV6
	else
		iRet = bind( m_iReadSock, ( struct sockaddr * ) m_bindhost.GetSockAddr6(), m_bindhost.GetSockAddrLen6() );
#endif /* HAVE_IPV6 */

	if( iRet == 0 )
	{
		if( m_eConState != CST_OK )
			m_eConState = CST_DESTDNS;
		return( true );
	}
	m_iCurBindCount++;
	if( m_iCurBindCount > 3 )
	{
		CS_DEBUG( "Failure to bind to " << m_sBindHost );
		return( false );
	}

	return( true );
}

#ifdef HAVE_LIBSSL
void Csock::FREE_SSL()
{
	if( m_ssl )
	{
		SSL_shutdown( m_ssl );
		SSL_free( m_ssl );
	}
	m_ssl = NULL;
}

void Csock::FREE_CTX()
{
	if( m_ssl_ctx )
		SSL_CTX_free( m_ssl_ctx );

	m_ssl_ctx = NULL;
}

#endif /* HAVE_LIBSSL */

cs_sock_t Csock::CreateSocket( bool bListen, bool bUnix )
{
	int domain, protocol;
	if ( !bUnix )
	{
#ifdef HAVE_IPV6
		domain = ( GetIPv6() ? PF_INET6 : PF_INET );
#else
		domain = PF_INET;
#endif /* HAVE_IPV6 */
		protocol = IPPROTO_TCP;
	}
	else
	{
#ifdef HAVE_UNIX_SOCKET
		domain = AF_UNIX;
		protocol = 0;
#else
		return CS_INVALID_SOCK;
#endif
	}
	cs_sock_t iRet = socket( domain, SOCK_STREAM, protocol );

	if( iRet != CS_INVALID_SOCK )
	{
		set_close_on_exec( iRet );

		if( bListen )
		{
			const int on = 1;
			if( setsockopt( iRet, SOL_SOCKET, SO_REUSEADDR, ( char * ) &on, sizeof( on ) ) != 0 )
				PERROR( "SO_REUSEADDR" );
		}
	}
	else
	{
		PERROR( "socket" );
	}
	return( iRet );
}

void Csock::Init( const CS_STRING & sHostname, uint16_t uPort, int iTimeout )
{
#ifdef HAVE_LIBSSL
	m_ssl = NULL;
	m_ssl_ctx = NULL;
	m_iRequireClientCertFlags = 0;
	m_uDisableProtocols = 0;
	m_bNoSSLCompression = false;
	m_bSSLCipherServerPreference = false;
#endif /* HAVE_LIBSSL */
	m_iTcount = 0;
	m_iReadSock = CS_INVALID_SOCK;
	m_iWriteSock = CS_INVALID_SOCK;
	m_iTimeout = iTimeout;
	m_iMaxConns = SOMAXCONN;
	m_bUseSSL = false;
	m_bIsConnected = false;
	m_uPort = uPort;
	m_shostname = sHostname;
	m_sbuffer.clear();
	m_eCloseType = CLT_DONT;
	/*
	 * While I appreciate the line ...
	 * "It's 2014, no idea how this made it as a default for the past 16 years..."
	 * TLS 1.2 was introduced in 2008. That being said, it's still not widely supported so I'm not
	 * ready to make it the default. SSL 3.0 is still the most widely supported standard and that's
	 * what a sane default is supposed to be. Additionally, OpenSSL is smart with SSLv23_client_method
	 * as it will check for TLS in addition to SSL (per the manual) which is the reason for its choice.
	 *
	 * https://www.openssl.org/docs/ssl/SSL_CTX_new.html
	 */
	m_iMethod = SSL23;
	m_sCipherType = "ALL";
	m_iMaxBytes = 0;
	m_iMaxMilliSeconds = 0;
	m_iLastSendTime = 0;
	m_iLastSend = 0;
	m_uSendBufferPos = 0;
	m_bsslEstablished = false;
	m_bEnableReadLine = false;
	m_iMaxStoredBufferLength = 1024;
	m_iConnType = INBOUND;
	m_iRemotePort = 0;
	m_iLocalPort = 0;
	m_iBytesRead = 0;
	m_iBytesWritten = 0;
	m_iStartTime = millitime();
	m_bPauseRead = false;
	m_iTimeoutType = TMO_ALL;
	m_eConState = CST_OK;	// default should be ok
	m_iDNSTryCount = 0;
	m_iCurBindCount = 0;
	m_bIsIPv6 = false;
	m_bSkipConnect = false;
	m_iLastCheckTimeoutTime = 0;
#ifdef HAVE_C_ARES
	m_pARESChannel = NULL;
	m_pCurrAddr = NULL;
	m_iARESStatus = -1;
#endif /* HAVE_C_ARES */
#ifdef HAVE_ICU
	m_cnvTryUTF8 = false;
	m_cnvSendUTF8 = false;
	UErrorCode e = U_ZERO_ERROR;
	m_cnvExt = NULL;
	m_cnvInt = ucnv_open( "UTF-8", &e );
#endif /* HAVE_ICU */
#ifdef HAVE_UNIX_SOCKET
	m_bUnixListen = false;
#endif
}

////////////////////////// CSocketManager //////////////////////////
CSocketManager::CSocketManager() : std::vector<Csock *>(), CSockCommon()
{
	m_errno = SUCCESS;
	m_iCallTimeouts = millitime();
	m_iSelectWait = 100000; // Default of 100 milliseconds
	m_iBytesRead = 0;
	m_iBytesWritten = 0;
}

CSocketManager::~CSocketManager()
{
	clear();
}

void CSocketManager::clear()
{
	while( !this->empty() )
		DelSock( 0 );
}

void CSocketManager::Cleanup()
{
	CleanupCrons();
	CleanupFDMonitors();
	clear();
}

Csock * CSocketManager::GetSockObj( const CS_STRING & sHostname, uint16_t uPort, int iTimeout )
{
	return( new Csock( sHostname, uPort, iTimeout ) );
}

void CSocketManager::Connect( const CSConnection & cCon, Csock * pcSock )
{
	// create the new object
	if( !pcSock )
		pcSock = GetSockObj( cCon.GetHostname(), cCon.GetPort(), cCon.GetTimeout() );
	else
	{
		pcSock->SetHostName( cCon.GetHostname() );
		pcSock->SetPort( cCon.GetPort() );
		pcSock->SetTimeout( cCon.GetTimeout() );
	}

	if( cCon.GetAFRequire() != CSSockAddr::RAF_ANY )
		pcSock->SetAFRequire( cCon.GetAFRequire() );

	// bind the vhost
	pcSock->SetBindHost( cCon.GetBindHost() );

#ifdef HAVE_LIBSSL
	pcSock->SetSSL( cCon.GetIsSSL() );
	if( cCon.GetIsSSL() )
	{
		if( !cCon.GetPemLocation().empty() )
		{
			pcSock->SetDHParamLocation( cCon.GetDHParamLocation() );
			pcSock->SetKeyLocation( cCon.GetKeyLocation() );
			pcSock->SetPemLocation( cCon.GetPemLocation() );
			pcSock->SetPemPass( cCon.GetPemPass() );
		}
		if( !cCon.GetCipher().empty() )
			pcSock->SetCipher( cCon.GetCipher() );
	}
#endif /* HAVE_LIBSSL */

	pcSock->SetType( Csock::OUTBOUND );

	pcSock->SetConState( Csock::CST_START );
	AddSock( pcSock, cCon.GetSockName() );
}

bool CSocketManager::Listen( const CSListener & cListen, Csock * pcSock, uint16_t * piRandPort )
{
	if( !pcSock )
		pcSock = GetSockObj( "", 0 );

	if( cListen.GetAFRequire() != CSSockAddr::RAF_ANY )
	{
		pcSock->SetAFRequire( cListen.GetAFRequire() );
#ifdef HAVE_IPV6
		if( cListen.GetAFRequire() == CSSockAddr::RAF_INET6 )
			pcSock->SetIPv6( true );
#endif /* HAVE_IPV6 */
	}
#ifdef HAVE_IPV6
	else
	{
		pcSock->SetIPv6( true );
	}
#endif /* HAVE_IPV6 */
#ifdef HAVE_LIBSSL
	pcSock->SetSSL( cListen.GetIsSSL() );
	if( cListen.GetIsSSL() && !cListen.GetPemLocation().empty() )
	{
		pcSock->SetDHParamLocation( cListen.GetDHParamLocation() );
		pcSock->SetKeyLocation( cListen.GetKeyLocation() );
		pcSock->SetPemLocation( cListen.GetPemLocation() );
		pcSock->SetPemPass( cListen.GetPemPass() );
		pcSock->SetCipher( cListen.GetCipher() );
		pcSock->SetRequireClientCertFlags( cListen.GetRequireClientCertFlags() );
	}
#endif /* HAVE_LIBSSL */

	if( piRandPort )
		*piRandPort = 0;

	bool bDetach = ( cListen.GetDetach() && !piRandPort ); // can't detach if we're waiting for the port to come up right now
	if( pcSock->Listen( cListen.GetPort(), cListen.GetMaxConns(), cListen.GetBindHost(), cListen.GetTimeout(), bDetach ) )
	{
		AddSock( pcSock, cListen.GetSockName() );
		if( piRandPort && cListen.GetPort() == 0 )
		{
			cs_sock_t iSock = pcSock->GetSock();

			if( iSock == CS_INVALID_SOCK )
			{
				CS_DEBUG( "Failed to attain a valid file descriptor" );
				pcSock->Close();
				return( false );
			}
			struct sockaddr_in mLocalAddr;
			socklen_t mLocalLen = sizeof( mLocalAddr );
			getsockname( iSock, ( struct sockaddr * ) &mLocalAddr, &mLocalLen );
			*piRandPort = ntohs( mLocalAddr.sin_port );
		}
		return( true );
	}

	CS_Delete( pcSock );
	return( false );
}


bool CSocketManager::HasFDs() const
{
	return( !this->empty() || !m_vcMonitorFD.empty() );
}

void CSocketManager::Loop()
{
	for( size_t a = 0; a < this->size(); ++a )
	{
		Csock * pcSock = this->at( a );
		if( pcSock->GetType() != Csock::OUTBOUND || pcSock->GetConState() == Csock::CST_OK )
			continue;
		if( pcSock->GetConState() == Csock::CST_DNS )
		{
			if( pcSock->DNSLookup( Csock::DNS_VHOST ) == ETIMEDOUT )
			{
				pcSock->CallSockError( EDOM, "DNS Lookup for bind host failed" );
				DelSock( a-- );
				continue;
			}
		}

		if( pcSock->GetConState() == Csock::CST_BINDVHOST )
		{
			if( !pcSock->SetupVHost() )
			{
				pcSock->CallSockError( GetSockError(), "Failed to setup bind host" );
				DelSock( a-- );
				continue;
			}
		}

		if( pcSock->GetConState() == Csock::CST_DESTDNS )
		{
			if( pcSock->DNSLookup( Csock::DNS_DEST ) == ETIMEDOUT )
			{
				pcSock->CallSockError( EADDRNOTAVAIL, "Unable to resolve requested address" );
				DelSock( a-- );
				continue;
			}
		}
		if( pcSock->GetConState() == Csock::CST_CONNECT )
		{
			if( !pcSock->Connect() )
			{
				if( GetSockError() == ECONNREFUSED )
					pcSock->ConnectionRefused();
				else
					pcSock->CallSockError( GetSockError() );

				DelSock( a-- );
				continue;
			}
		}
#ifdef HAVE_LIBSSL
		if( pcSock->GetConState() == Csock::CST_CONNECTSSL )
		{
			if( pcSock->GetSSL() )
			{
				if( !pcSock->ConnectSSL() )
				{
					if( GetSockError() == ECONNREFUSED )
						pcSock->ConnectionRefused();
					else
						pcSock->CallSockError( GetSockError() == 0 ? ECONNABORTED : GetSockError() );

					DelSock( a-- );
					continue;
				}
			}
		}
#endif /* HAVE_LIBSSL */
	}

	std::map<Csock *, EMessages> mpeSocks;
	Select( mpeSocks );

	switch( m_errno )
	{
	case SUCCESS:
	{
		for( std::map<Csock *, EMessages>::iterator itSock = mpeSocks.begin(); itSock != mpeSocks.end(); itSock++ )
		{
			Csock * pcSock = itSock->first;
			EMessages iErrno = itSock->second;

			if( iErrno == SUCCESS )
			{
				// read in data
				// if this is a
				int iLen = 0;

				if( pcSock->GetSSL() )
					iLen = pcSock->GetPending();

				if( iLen <= 0 )
					iLen = CS_BLOCKSIZE;

				CSCharBuffer cBuff( iLen );

				cs_ssize_t bytes = pcSock->Read( cBuff(), iLen );

				if( bytes != Csock::READ_TIMEDOUT && bytes != Csock::READ_CONNREFUSED && bytes != Csock::READ_ERR && !pcSock->IsConnected() )
				{
					pcSock->SetIsConnected( true );
					pcSock->Connected();
				}

				switch( bytes )
				{
					case Csock::READ_EOF:
					{
						DelSockByAddr( pcSock );
						break;
					}

					case Csock::READ_ERR:
					{
						bool bHandled = false;
#ifdef HAVE_LIBSSL
						if( pcSock->GetSSL() )
						{
							unsigned long iSSLError = ERR_peek_error();
							if( iSSLError )
							{
								char szError[512];
								memset( ( char * ) szError, '\0', 512 );
								ERR_error_string_n( iSSLError, szError, 511 );
								SSLErrors( __FILE__, __LINE__ );
								pcSock->CallSockError( GetSockError(), szError );
								bHandled = true;
							}
						}
#endif
						if( !bHandled )
							pcSock->CallSockError( GetSockError() );
						DelSockByAddr( pcSock );
						break;
					}

					case Csock::READ_EAGAIN:
						break;

					case Csock::READ_CONNREFUSED:
						pcSock->ConnectionRefused();
						DelSockByAddr( pcSock );
						break;

					case Csock::READ_TIMEDOUT:
						pcSock->Timeout();
						DelSockByAddr( pcSock );
						break;

					default:
					{
						if( Csock::TMO_READ & pcSock->GetTimeoutType() )
							pcSock->ResetTimer();	// reset the timeout timer

						pcSock->ReadData( cBuff(), bytes );	// Call ReadData() before PushBuff() so that it is called before the ReadLine() event - LD  07/18/05
						pcSock->PushBuff( cBuff(), bytes );
						break;
					}
				}
			}
			else if( iErrno == SELECT_ERROR )
			{
				// a socket came back with an error
				// usually means it was closed
				DelSockByAddr( pcSock );
			}
		}
		break;
	}

	case SELECT_TIMEOUT:
	case SELECT_TRYAGAIN:
	case SELECT_ERROR:
	default	:
		break;
	}

	uint64_t iMilliNow = millitime();
	if( ( iMilliNow - m_iCallTimeouts ) >= 1000 )
	{
		m_iCallTimeouts = iMilliNow;
		// call timeout on all the sockets that recieved no data
		for( size_t i = 0; i < this->size(); ++i )
		{
			if( this->at( i )->GetConState() != Csock::CST_OK )
				continue;

			if( this->at( i )->CheckTimeout( ( time_t )( iMilliNow / 1000 ) ) )
				DelSock( i-- );
		}
	}
	// run any Manager Crons we may have
	Cron();
}

void CSocketManager::DynamicSelectLoop( uint64_t iLowerBounds, uint64_t iUpperBounds, time_t iMaxResolution )
{
	SetSelectTimeout( iLowerBounds );
	if( m_errno == SELECT_TIMEOUT )
	{
		// only do this if the previous call to select was a timeout
		timeval tMaxResolution;
		timeval tNow;
		tMaxResolution.tv_sec = iMaxResolution;
		tMaxResolution.tv_usec = 0;
		CS_GETTIMEOFDAY( &tNow, NULL );
		timeval tSelectTimeout = GetDynamicSleepTime( tNow, tMaxResolution );
		uint64_t iSelectTimeout = tSelectTimeout.tv_sec * 1000000 + tSelectTimeout.tv_usec;
		iSelectTimeout = std::max( iLowerBounds, iSelectTimeout );
		iSelectTimeout = std::min( iSelectTimeout, iUpperBounds );
		if( iLowerBounds != iSelectTimeout )
			SetSelectTimeout( iSelectTimeout );
	}
	Loop();
}

void CSocketManager::AddSock( Csock * pcSock, const CS_STRING & sSockName )
{
	pcSock->SetSockName( sSockName );
	this->push_back( pcSock );
}

Csock * CSocketManager::FindSockByRemotePort( uint16_t iPort )
{
	for( size_t i = 0; i < this->size(); ++i )
	{
		if( this->at( i )->GetRemotePort() == iPort )
			return( this->at( i ) );
	}
	return( NULL );
}

Csock * CSocketManager::FindSockByLocalPort( uint16_t iPort )
{
	for( size_t i = 0; i < this->size(); ++i )
	{
		if( this->at( i )->GetLocalPort() == iPort )
			return( this->at( i ) );
	}
	return( NULL );
}

Csock * CSocketManager::FindSockByName( const CS_STRING & sName )
{
	std::vector<Csock *>::iterator it;
	std::vector<Csock *>::iterator it_end = this->end();
	for( it = this->begin(); it != it_end; it++ )
	{
		if( ( *it )->GetSockName() == sName )
			return( *it );
	}
	return( NULL );
}

Csock * CSocketManager::FindSockByFD( cs_sock_t iFD )
{
	for( size_t i = 0; i < this->size(); ++i )
	{
		if( this->at( i )->GetRSock() == iFD || this->at( i )->GetWSock() == iFD )
			return( this->at( i ) );
	}
	return( NULL );
}

std::vector<Csock *> CSocketManager::FindSocksByName( const CS_STRING & sName )
{
	std::vector<Csock *> vpSocks;
	for( size_t i = 0; i < this->size(); ++i )
	{
		if( this->at( i )->GetSockName() == sName )
			vpSocks.push_back( this->at( i ) );
	}
	return( vpSocks );
}

std::vector<Csock *> CSocketManager::FindSocksByRemoteHost( const CS_STRING & sHostname )
{
	std::vector<Csock *> vpSocks;
	for( size_t i = 0; i < this->size(); ++i )
	{
		if( this->at( i )->GetHostName() == sHostname )
			vpSocks.push_back( this->at( i ) );
	}
	return( vpSocks );
}

void CSocketManager::DelSockByAddr( Csock * pcSock )
{
	for( size_t a = 0; a < this->size(); ++a )
	{
		if( pcSock == this->at( a ) )
		{
			DelSock( a );
			return;
		}
	}
}

void CSocketManager::DelSock( size_t iPos )
{
	if( iPos >= this->size() )
	{
		CS_DEBUG( "Invalid Sock Position Requested! [" << iPos << "]" );
		return;
	}

	Csock * pSock = this->at( iPos );

	if( pSock->GetCloseType() != Csock::CLT_DEREFERENCE )
	{
		if( pSock->IsConnected() )
		{
			pSock->SetIsConnected( false );
			pSock->Disconnected(); // only call disconnected event if connected event was called (IE IsConnected was set)
		}

		m_iBytesRead += pSock->GetBytesRead();
		m_iBytesWritten += pSock->GetBytesWritten();
	}

	CS_Delete( pSock );
	this->erase( this->begin() + iPos );
}

bool CSocketManager::SwapSockByIdx( Csock * pNewSock, size_t iOrginalSockIdx )
{
	if( iOrginalSockIdx >= this->size() )
	{
		CS_DEBUG( "Invalid Sock Position Requested! [" << iOrginalSockIdx << "]" );
		return( false );
	}

	Csock * pSock = this->at( iOrginalSockIdx );
	pNewSock->Copy( *pSock );
	pSock->Dereference();
	this->at( iOrginalSockIdx ) = ( Csock * )pNewSock;
	this->push_back( ( Csock * )pSock ); // this allows it to get cleaned up
	return( true );
}

bool CSocketManager::SwapSockByAddr( Csock * pNewSock, Csock * pOrigSock )
{
	for( size_t a = 0; a < this->size(); ++a )
	{
		if( this->at( a ) == pOrigSock )
			return( SwapSockByIdx( pNewSock, a ) );
	}
	return( false );
}

uint64_t CSocketManager::GetBytesRead() const
{
	// Start with the total bytes read from destroyed sockets
	uint64_t iRet = m_iBytesRead;

	// Add in the outstanding bytes read from active sockets
	for( size_t a = 0; a < this->size(); ++a )
		iRet += this->at( a )->GetBytesRead();

	return( iRet );
}

uint64_t CSocketManager::GetBytesWritten() const
{
	// Start with the total bytes written to destroyed sockets
	uint64_t iRet = m_iBytesWritten;

	// Add in the outstanding bytes written to active sockets
	for( size_t a = 0; a < this->size(); ++a )
		iRet += this->at( a )->GetBytesWritten();

	return( iRet );
}

void CSocketManager::FDSetCheck( cs_sock_t iFd, std::map< cs_sock_t, short > & miiReadyFds, ECheckType eType )
{
	std::map< cs_sock_t, short >::iterator it = miiReadyFds.find( iFd );
	if( it != miiReadyFds.end() )
		it->second = ( short )( it->second | eType ); // TODO need to figure out why |= throws 'short int' from 'int' may alter its value
	else
		miiReadyFds[iFd] = eType;
}

bool CSocketManager::FDHasCheck( cs_sock_t iFd, std::map< cs_sock_t, short > & miiReadyFds, ECheckType eType )
{
	std::map< cs_sock_t, short >::iterator it = miiReadyFds.find( iFd );
	if( it != miiReadyFds.end() )
		return( ( it->second & eType ) != 0 );
	return( false );
}

int CSocketManager::Select( std::map< cs_sock_t, short > & miiReadyFds, struct timeval *tvtimeout )
{
	AssignFDs( miiReadyFds, tvtimeout );
#ifdef CSOCK_USE_POLL
	if( miiReadyFds.empty() )
		return( select( 0, NULL, NULL, NULL, tvtimeout ) );

	struct pollfd * pFDs = ( struct pollfd * )malloc( sizeof( struct pollfd ) * miiReadyFds.size() );
	size_t uCurrPoll = 0;
	for( std::map< cs_sock_t, short >::iterator it = miiReadyFds.begin(); it != miiReadyFds.end(); ++it, ++uCurrPoll )
	{
		short iEvents = 0;
		if( it->second & ECT_Read )
			iEvents |= POLLIN;
		if( it->second & ECT_Write )
			iEvents |= POLLOUT;
		pFDs[uCurrPoll].fd = it->first;
		pFDs[uCurrPoll].events = iEvents;
		pFDs[uCurrPoll].revents = 0;
	}
	int iTimeout = ( int )( tvtimeout->tv_usec / 1000 );
	iTimeout += ( int )( tvtimeout->tv_sec * 1000 );
	size_t uMaxFD = miiReadyFds.size();
	int iRet = poll( pFDs, uMaxFD, iTimeout );
	miiReadyFds.clear();
	for( uCurrPoll = 0; uCurrPoll < uMaxFD; ++uCurrPoll )
	{
		short iEvents = 0;
		if( pFDs[uCurrPoll].revents & ( POLLIN|POLLERR|POLLHUP|POLLNVAL ) )
			iEvents |= ECT_Read;
		if( pFDs[uCurrPoll].revents & POLLOUT )
			iEvents |= ECT_Write;
		std::map< cs_sock_t, short >::iterator it = miiReadyFds.find( pFDs[uCurrPoll].fd );
		if( it != miiReadyFds.end() )
			it->second = ( short )( it->second | iEvents ); // TODO need to figure out why |= throws 'short int' from 'int' may alter its value
		else
			miiReadyFds[pFDs[uCurrPoll].fd] = iEvents;
	}
	free( pFDs );
#else
	fd_set rfds, wfds;
	TFD_ZERO( &rfds );
	TFD_ZERO( &wfds );
	bool bHasWrite = false;
	int iHighestFD = 0;
	for( std::map< cs_sock_t, short >::iterator it = miiReadyFds.begin(); it != miiReadyFds.end(); ++it )
	{
#ifndef _WIN32
		// the first argument to select() is not used on Win32.
		iHighestFD = std::max( it->first, iHighestFD );
#endif /* _WIN32 */
		if( it->second & ECT_Read )
		{
			TFD_SET( it->first, &rfds );
		}
		if( it->second & ECT_Write )
		{
			bHasWrite = true;
			TFD_SET( it->first, &wfds );
		}
	}

	int iRet = select( iHighestFD + 1, &rfds, ( bHasWrite ? &wfds : NULL ), NULL, tvtimeout );
	if( iRet <= 0 )
	{
		miiReadyFds.clear();
	}
	else
	{
		for( std::map< cs_sock_t, short >::iterator it = miiReadyFds.begin(); it != miiReadyFds.end(); ++it )
		{
			if( ( it->second & ECT_Read ) && !TFD_ISSET( it->first, &rfds ) )
				it->second &= ~ECT_Read;
			if( ( it->second & ECT_Write ) && !TFD_ISSET( it->first, &wfds ) )
				it->second &= ~ECT_Write;
		}
	}
#endif /* CSOCK_USE_POLL */

	return( iRet );
}

void CSocketManager::Select( std::map<Csock *, EMessages> & mpeSocks )
{
	mpeSocks.clear();
	struct timeval tv;

	std::map< cs_sock_t, short > miiReadyFds;
	tv.tv_sec = ( time_t )( m_iSelectWait / 1000000 );
	tv.tv_usec = ( time_t )( m_iSelectWait % 1000000 );
	u_int iQuickReset = 1000;
	if( m_iSelectWait == 0 )
		iQuickReset = 0;

	bool bHasAvailSocks = false;
	uint64_t iNOW = 0;
	for( size_t i = 0; i < this->size(); ++i )
	{
		Csock * pcSock = this->at( i );

		Csock::ECloseType eCloseType = pcSock->GetCloseType();

		if( eCloseType == Csock::CLT_NOW || eCloseType == Csock::CLT_DEREFERENCE || ( eCloseType == Csock::CLT_AFTERWRITE && !pcSock->HasWriteBuffer() ) )
		{
			DelSock( i-- ); // close any socks that have requested it
			continue;
		}
		else
		{
			pcSock->Cron(); // call the Cron handler here
		}

		cs_sock_t & iRSock = pcSock->GetRSock();
		cs_sock_t & iWSock = pcSock->GetWSock();
#if !defined(CSOCK_USE_POLL) && !defined(_WIN32)
		if( iRSock > ( cs_sock_t )FD_SETSIZE || iWSock > ( cs_sock_t )FD_SETSIZE )
		{
			CS_DEBUG( "FD is larger than select() can handle" );
			DelSock( i-- );
			continue;
		}
#endif /* CSOCK_USE_POLL */

#ifdef HAVE_C_ARES
		ares_channel pChannel = pcSock->GetAresChannel();
		if( pChannel )
		{
			ares_socket_t aiAresSocks[1];
			aiAresSocks[0] = ARES_SOCKET_BAD;
			int iSockMask = ares_getsock( pChannel, aiAresSocks, 1 );
			if( ARES_GETSOCK_READABLE( iSockMask, 0 ) )
				FDSetCheck( aiAresSocks[0], miiReadyFds, ECT_Read );
			if( ARES_GETSOCK_WRITABLE( iSockMask, 0 ) )
				FDSetCheck( aiAresSocks[0], miiReadyFds, ECT_Write );
			// let ares drop the timeout if it has something timing out sooner then whats in tv currently
			ares_timeout( pChannel, &tv, &tv );
		}
#endif /* HAVE_C_ARES */
		if( pcSock->GetType() == Csock::LISTENER && pcSock->GetConState() == Csock::CST_BINDVHOST )
		{
			if( !pcSock->Listen( pcSock->GetPort(), pcSock->GetMaxConns(), pcSock->GetBindHost(), pcSock->GetTimeout(), true ) )
			{
				pcSock->Close();
				DelSock( i-- );
			}
			continue;
		}

		pcSock->AssignFDs( miiReadyFds, &tv );

		if( pcSock->GetConState() != Csock::CST_OK && pcSock->GetConState() != Csock::CST_CONNECTWAIT )
			continue;

		bHasAvailSocks = true;

		bool bIsReadPaused = pcSock->IsReadPaused();
		if( bIsReadPaused )
		{
			pcSock->ReadPaused();
			bIsReadPaused = pcSock->IsReadPaused(); // re-read it again, incase it changed status)
		}
		if( iRSock == CS_INVALID_SOCK || iWSock == CS_INVALID_SOCK )
		{
			SelectSock( mpeSocks, SUCCESS, pcSock );
			continue;	// invalid sock fd
		}

		if( pcSock->GetType() != Csock::LISTENER )
		{
			bool bHasWriteBuffer = pcSock->HasWriteBuffer();

			if( !bIsReadPaused )
				FDSetCheck( iRSock, miiReadyFds, ECT_Read );

			if( pcSock->AllowWrite( iNOW ) && ( !pcSock->IsConnected() || bHasWriteBuffer ) )
			{
				if( !pcSock->IsConnected() )
				{
					// set the write bit if not connected yet
					FDSetCheck( iWSock, miiReadyFds, ECT_Write );
				}
				else if( bHasWriteBuffer && !pcSock->GetSSL() )
				{
					// always set the write bit if there is data to send when NOT ssl
					FDSetCheck( iWSock, miiReadyFds, ECT_Write );
				}
				else if( bHasWriteBuffer && pcSock->GetSSL() && pcSock->SslIsEstablished() )
				{
					// ONLY set the write bit if there is data to send and the SSL handshake is finished
					FDSetCheck( iWSock, miiReadyFds, ECT_Write );
				}
			}

			if( pcSock->GetSSL() && !pcSock->SslIsEstablished() && bHasWriteBuffer )
			{
				// if this is an unestabled SSL session with data to send ... try sending it
				// do this here, cause otherwise ssl will cause a small
				// cpu spike waiting for the handshake to finish
				// resend this data
				if( !pcSock->Write( "" ) )
				{
					pcSock->Close();
				}
				// warning ... setting write bit in here causes massive CPU spinning on invalid SSL servers
				// http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=631590
				// however, we can set the select WAY down and it will retry quickly, but keep it from spinning at 100%
				tv.tv_usec = iQuickReset;
				tv.tv_sec = 0;
			}
		}
		else
		{
			FDSetCheck( iRSock, miiReadyFds, ECT_Read );
		}

		if( pcSock->GetSSL() && pcSock->GetType() != Csock::LISTENER )
		{
			if( pcSock->GetPending() > 0 && !pcSock->IsReadPaused() )
				SelectSock( mpeSocks, SUCCESS, pcSock );
		}
	}

	// old fashion select, go fer it
	int iSel;

	if( !mpeSocks.empty() ) // .1 ms pause to see if anything else is ready (IE if there is SSL data pending, don't wait too long)
	{
		tv.tv_usec = iQuickReset;
		tv.tv_sec = 0;
	}
	else if( !this->empty() && !bHasAvailSocks )
	{
		tv.tv_usec = iQuickReset;
		tv.tv_sec = 0;
	}

	iSel = Select( miiReadyFds, &tv );

	if( iSel == 0 )
	{
		if( mpeSocks.empty() )
			m_errno = SELECT_TIMEOUT;
		else
			m_errno = SUCCESS;
#ifdef HAVE_C_ARES
		// run through ares channels and process timeouts
		for( size_t uSock = 0; uSock < this->size(); ++uSock )
		{
			Csock * pcSock = this->at( uSock );
			ares_channel pChannel = pcSock->GetAresChannel();
			if( pChannel )
				ares_process_fd( pChannel, ARES_SOCKET_BAD, ARES_SOCKET_BAD );
		}
#endif /* HAVE_C_ARES */

		return;
	}

	if( iSel == -1 && errno == EINTR )
	{
		if( mpeSocks.empty() )
			m_errno = SELECT_TRYAGAIN;
		else
			m_errno = SUCCESS;

		return;
	}
	else if( iSel == -1 )
	{
		if( mpeSocks.empty() )
			m_errno = SELECT_ERROR;
		else
			m_errno = SUCCESS;

		return;
	}
	else
	{
		m_errno = SUCCESS;
	}

	CheckFDs( miiReadyFds );

	// find out wich one is ready
	for( size_t i = 0; i < this->size(); ++i )
	{
		Csock * pcSock = this->at( i );

#ifdef HAVE_C_ARES
		ares_channel pChannel = pcSock->GetAresChannel();
		if( pChannel )
		{
			ares_socket_t aiAresSocks[1];
			aiAresSocks[0] = ARES_SOCKET_BAD;
			ares_getsock( pChannel, aiAresSocks, 1 );
			if( FDHasCheck( aiAresSocks[0], miiReadyFds, ECT_Read ) || FDHasCheck( aiAresSocks[0], miiReadyFds, ECT_Write ) )
				ares_process_fd( pChannel, aiAresSocks[0], aiAresSocks[0] );
		}
#endif /* HAVE_C_ARES */
		pcSock->CheckFDs( miiReadyFds );

		if( pcSock->GetConState() != Csock::CST_OK && pcSock->GetConState() != Csock::CST_CONNECTWAIT )
			continue;

		cs_sock_t & iRSock = pcSock->GetRSock();
		cs_sock_t & iWSock = pcSock->GetWSock();
		EMessages iErrno = SUCCESS;

		if( iRSock == CS_INVALID_SOCK || iWSock == CS_INVALID_SOCK )
		{
			// trigger a success so it goes through the normal motions
			// and an error is produced
			SelectSock( mpeSocks, SUCCESS, pcSock );
			continue; // watch for invalid socks
		}

		if( FDHasCheck( iWSock, miiReadyFds, ECT_Write ) )
		{
			if( iSel > 0 )
			{
				iErrno = SUCCESS;
				if( pcSock->HasWriteBuffer() && pcSock->IsConnected() )
				{
					// write whats in the socks send buffer
					if( !pcSock->Write( "" ) )
					{
						// write failed, sock died :(
						iErrno = SELECT_ERROR;
					}
				}
			}
			else
			{
				iErrno = SELECT_ERROR;
			}

			SelectSock( mpeSocks, iErrno, pcSock );

		}
		else if( FDHasCheck( iRSock, miiReadyFds, ECT_Read ) )
		{
			if( iSel > 0 )
				iErrno = SUCCESS;
			else
				iErrno = SELECT_ERROR;

			if( pcSock->GetType() != Csock::LISTENER )
			{
				SelectSock( mpeSocks, iErrno, pcSock );
			}
			else // someone is coming in!
			{
				CS_STRING sHost;
				uint16_t port;
				cs_sock_t inSock = pcSock->Accept( sHost, port );

				if( inSock != CS_INVALID_SOCK )
				{
					if( Csock::TMO_ACCEPT & pcSock->GetTimeoutType() )
						pcSock->ResetTimer();	// let them now it got dinged

					// if we have a new sock, then add it
					Csock * NewpcSock = ( Csock * )pcSock->GetSockObj( sHost, port );

					if( !NewpcSock )
						NewpcSock = GetSockObj( sHost, port );

					NewpcSock->SetType( Csock::INBOUND );
					NewpcSock->SetRSock( inSock );
					NewpcSock->SetWSock( inSock );
					NewpcSock->SetIPv6( pcSock->GetIPv6() );

					bool bAddSock = true;
#ifdef HAVE_LIBSSL
					//
					// is this ssl ?
					if( pcSock->GetSSL() )
					{
						NewpcSock->SetCipher( pcSock->GetCipher() );
						NewpcSock->SetDHParamLocation( pcSock->GetDHParamLocation() );
						NewpcSock->SetKeyLocation( pcSock->GetKeyLocation() );
						NewpcSock->SetPemLocation( pcSock->GetPemLocation() );
						NewpcSock->SetPemPass( pcSock->GetPemPass() );
						NewpcSock->SetRequireClientCertFlags( pcSock->GetRequireClientCertFlags() );
						bAddSock = NewpcSock->AcceptSSL();
					}

#endif /* HAVE_LIBSSL */
					if( bAddSock )
					{
						// set the name of the listener
						NewpcSock->SetParentSockName( pcSock->GetSockName() );
						NewpcSock->SetRate( pcSock->GetRateBytes(), pcSock->GetRateTime() );
#ifdef HAVE_ICU
						NewpcSock->SetEncoding( pcSock->GetEncoding() );
#endif
						if( NewpcSock->GetSockName().empty() )
						{
							std::stringstream s;
							s << sHost << ":" << port;
							AddSock( NewpcSock,  s.str() );
						}
						else
						{
							AddSock( NewpcSock, NewpcSock->GetSockName() );
						}
					}
					else
					{
						CS_Delete( NewpcSock );
					}
				}
#ifdef _WIN32
				else if( GetSockError() != WSAEWOULDBLOCK )
#else /* _WIN32 */
				else if( GetSockError() != EAGAIN )
#endif /* _WIN32 */
				{
					pcSock->CallSockError( GetSockError() );
				}
			}
		}
	}
}

inline void MinimizeTime( timeval& min, const timeval& another )
{
	if( timercmp( &min, &another, > ) )
	{
		min = another;
	}
}

timeval CSocketManager::GetDynamicSleepTime( const timeval& tNow, const timeval& tMaxResolution ) const
{
	timeval tNextRunTime;
	timeradd( &tNow, &tMaxResolution, &tNextRunTime );
	std::vector<Csock *>::const_iterator it;
	// This is safe, because we don't modify the vector.
	std::vector<Csock *>::const_iterator it_end = this->end();
	for( it = this->begin(); it != it_end; ++it )
	{
		Csock* pSock = *it;

		if( pSock->GetConState() != Csock::CST_OK )
			tNextRunTime = tNow; // this is in a nebulous state, need to let it proceed like normal

		time_t iTimeoutInSeconds = pSock->GetTimeout();
		if( iTimeoutInSeconds > 0 )
		{
			timeval tNextTimeout;
			tNextTimeout.tv_sec = pSock->GetNextCheckTimeout( 0 ); // TODO convert socket timeouts to timeval too?
			tNextTimeout.tv_usec = 0;
			MinimizeTime( tNextRunTime, tNextTimeout );
		}

		const std::vector<CCron *> & vCrons = pSock->GetCrons();
		std::vector<CCron *>::const_iterator cit;
		std::vector<CCron *>::const_iterator cit_end = vCrons.end();
		for( cit = vCrons.begin(); cit != cit_end; ++cit )
			MinimizeTime( tNextRunTime, ( *cit )->GetNextRun() );
	}
	std::vector<CCron *>::const_iterator cit;
	std::vector<CCron *>::const_iterator cit_end = m_vcCrons.end();
	for( cit = m_vcCrons.begin(); cit != cit_end; ++cit )
		MinimizeTime( tNextRunTime, ( *cit )->GetNextRun() );

	timeval tReturnValue;
	if( timercmp( &tNextRunTime, &tNow, < ) )
	{
		timerclear( &tReturnValue );
		return( tReturnValue ); // smallest unit possible
	}
	timersub( &tNextRunTime, &tNow, &tReturnValue );
	MinimizeTime( tReturnValue, tMaxResolution );
	return( tReturnValue );
}

void CSocketManager::SelectSock( std::map<Csock *, EMessages> & mpeSocks, EMessages eErrno, Csock * pcSock )
{
	if( mpeSocks.find( pcSock ) != mpeSocks.end() )
		return;

	mpeSocks[pcSock] = eErrno;
}

