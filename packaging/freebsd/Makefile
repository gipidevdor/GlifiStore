PORTNAME=       glifistore
DISTVERSION=    0.1.0
CATEGORIES=     databases
MASTER_SITES=   https://github.com/gpicchiarelli/GlifiStore/releases/download/v${DISTVERSION}/
DISTNAME=       GlifiStore-${DISTVERSION}
EXTRACT_SUFX=   .tar.xz

MAINTAINER=     gpicchiarelli@gmail.com
COMMENT=        Paired-reader/writer in-memory key-value storage prototype
WWW=            https://github.com/gpicchiarelli/GlifiStore

LICENSE=        BSD3CLAUSE
LICENSE_FILE=   ${WRKSRC}/LICENSE

USES=           cmake:testing compiler:c++23-lang pathfix pkgconfig ssl
USE_LDCONFIG=   yes
USE_RC_SUBR=    glifistored
USERS=          glifistore
GROUPS=         glifistore

CMAKE_ON=       GLIFISTORE_WARNINGS_AS_ERRORS
CMAKE_OFF=      GLIFISTORE_BUILD_BENCHMARKS GLIFISTORE_BUILD_FUZZERS \
                GLIFISTORE_FAULT_INJECTION GLIFISTORE_NATIVE_CPU
CMAKE_ARGS=     -DGLIFISTORE_ENABLE_TLS=ON

TEST_TARGET=    test

post-install:
	${INSTALL_DATA} ${FILESDIR}/glifistored.conf.sample \
		${STAGEDIR}${PREFIX}/etc/glifistored.conf.sample
	${MKDIR} ${STAGEDIR}/var/db/glifistore

.include <bsd.port.mk>
