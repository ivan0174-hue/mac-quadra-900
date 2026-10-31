/*
 *  config.h - Build configuration for QuadraPi (Basilisk II on Circle, bare metal)
 */

#ifndef CONFIG_H
#define CONFIG_H

#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 8
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 16

#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRDUP 1
#define HAVE_CADDR_T 1

#define HOST_FLOAT_FORMAT IEEE_FLOAT_FORMAT

#define PACKAGE "QuadraPi"
#define PACKAGE_NAME "QuadraPi"
#define PACKAGE_VERSION "1.0"

/* Emulation model */
#ifndef DIRECT_ADDRESSING
#define DIRECT_ADDRESSING 1
#endif
#define USE_CPU_EMUL_SERVICES 1

#endif
