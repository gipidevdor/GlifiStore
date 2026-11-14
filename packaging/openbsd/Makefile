V =             0.1.0
COMMENT =       paired-reader/writer key-value storage prototype
DISTNAME =      GlifiStore-${V}
PKGNAME =       glifistore-${V}
CATEGORIES =    databases

HOMEPAGE =      https://github.com/gpicchiarelli/GlifiStore
MASTER_SITES =  https://github.com/gpicchiarelli/GlifiStore/releases/download/v${V}/
EXTRACT_SUFX =  .tar.xz

MAINTAINER =    Giacomo Picchiarelli <gpicchiarelli@gmail.com>
PERMIT_PACKAGE = Yes

# ABI_VERSION authority: validated against ../../ABI_VERSION by
# engineering/tools/validate_bsd_packaging.py.
SHARED_LIBS +=  glifistore 1.1

WANTLIB +=      c c++ c++abi crypto m pthread ssl

MODULES =       devel/cmake
BUILD_DEPENDS = devel/ninja
CONFIGURE_ARGS += -DGLIFISTORE_ENABLE_TLS=ON \
                  -DGLIFISTORE_BUILD_BENCHMARKS=OFF \
                  -DGLIFISTORE_BUILD_FUZZERS=OFF \
                  -DGLIFISTORE_FAULT_INJECTION=OFF \
                  -DGLIFISTORE_NATIVE_CPU=OFF \
                  -DGLIFISTORE_WARNINGS_AS_ERRORS=ON \
                  -DCMAKE_INSTALL_MANDIR=man

post-install:
	${INSTALL_DATA_DIR} ${PREFIX}/share/examples/glifistore
	${INSTALL_DATA} ${FILESDIR}/glifistored.conf \
		${PREFIX}/share/examples/glifistore/glifistored.conf

.include <bsd.port.mk>
