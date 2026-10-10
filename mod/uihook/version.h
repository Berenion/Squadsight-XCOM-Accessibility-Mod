// The mod's version, the one number the launcher, the DLL's log banner, the
// launcher's file properties and the release tag (v0.9.0) all share.  Bump it
// here before packaging a release (package.bat): the launcher offers an update
// only when GitHub's latest release tag is higher than this.
//
// The string and the three numbers say the same thing twice because the
// resource compiler cannot stringize; test_release checks that they agree.
// (Not "test_update": Windows wants administrator rights for an exe without a
// manifest whose name has "update", "install" or "setup" in it.)
#ifndef VERSION_H
#define VERSION_H

#define MOD_VERSION       "0.10.0"
#define MOD_VERSION_MAJOR 0
#define MOD_VERSION_MINOR 10
#define MOD_VERSION_PATCH 0

#define MOD_NAME    "Squadsight"
#define MOD_REPO    "Berenion/Squadsight-XCOM-Accessibility-Mod"

#endif
