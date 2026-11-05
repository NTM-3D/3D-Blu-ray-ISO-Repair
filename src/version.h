/* The only place the version is set; the code, the Windows resource and the
   build scripts all read it from here. */
#define BR3D_VER_MAJOR 0
#define BR3D_VER_MINOR 1
#define BR3D_VER_PATCH 0

#define BR3D_STR_(x) #x
#define BR3D_STR(x) BR3D_STR_(x)
#define BR3D_VERSION BR3D_STR(BR3D_VER_MAJOR.BR3D_VER_MINOR)
#define BR3D_VERSION_FULL BR3D_STR(BR3D_VER_MAJOR.BR3D_VER_MINOR.BR3D_VER_PATCH)
#define BR3D_CAT_(a, b) a##b
#define BR3D_CAT(a, b) BR3D_CAT_(a, b)
#define BR3D_EXE_NAME BR3D_STR(BR3D_CAT(3d_bluray_iso_repair_v, BR3D_VER_MAJOR).BR3D_VER_MINOR.exe)