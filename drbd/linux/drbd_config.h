/*
  drbd_config.h
  DRBD's compile time configuration.

  drbd is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  drbd is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with drbd; see the file COPYING.  If not, write to
  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef DRBD_CONFIG_H
#define DRBD_CONFIG_H

extern const char *drbd_buildtag(void);

/* Necessary to build the external module against >= Linux-2.6.33 */
#ifdef REL_VERSION
#undef REL_VERSION
#undef API_VERSION
#undef PRO_VERSION_MIN
#undef PRO_VERSION_MAX
#endif

/* End of external module for 2.6.33 stuff */

#define REL_VERSION "8.3.16"
#define API_VERSION 88
#define PRO_VERSION_MIN 86
#define PRO_VERSION_MAX 97

#ifndef __CHECKER__   /* for a sparse run, we need all STATICs */
#define DBG_ALL_SYMBOLS /* no static functs, improves quality of OOPS traces */
#endif

/* drbd_assert_breakpoint() function
#define DBG_ASSERTS
 */

/* Dump all cstate changes */
#define DUMP_MD 2

/* some extra checks
#define PARANOIA
 */

/* Enable fault insertion code */
#define DRBD_ENABLE_FAULTS

/* RedHat's 2.6.9 kernels have the gfp_t type. Mainline has this feature
 * since 2.6.16. If you build for RedHat enable the line below. */
#define KERNEL_HAS_GFP_T

/* kernel.org has atomic_add_return since 2.6.10. some vendor kernels
 * have it backported, though. Others don't. */
//#define NEED_BACKPORT_OF_ATOMIC_ADD

/* 2.6.something has deprecated kmem_cache_t
 * some older still use it.
 * some have it defined as struct kmem_cache_s, some as struct kmem_cache */
//#define USE_KMEM_CACHE_S

/* 2.6.something has sock_create_kern (SE-linux security context stuff)
 * some older distribution kernels don't. */
//#define DEFINE_SOCK_CREATE_KERN

/* 2.6.24 and later have kernel_sock_shutdown.
 * some older distribution kernels may also have a backport. */
//#define DEFINE_KERNEL_SOCK_SHUTDOWN

/* in older kernels (vanilla < 2.6.16) struct netlink_skb_parms has a
 * member called dst_groups. Later it is called dst_group (without 's'). */
//#define DRBD_NL_DST_GROUPS

/* in older kernels (vanilla < 2.6.14) is no kzalloc() */
//#define NEED_BACKPORT_OF_KZALLOC

// some vendor kernels have it, some don't
//#define NEED_SG_SET_BUF
#define HAVE_LINUX_SCATTERLIST_H

/* 2.6.29 and up no longer have swabb.h */
//#define HAVE_LINUX_BYTEORDER_SWABB_H

/* some vendor kernel have it backported. */
#define HAVE_SET_CPUS_ALLOWED_PTR

/* Some vendor kernels < 2.6.7 might define msleep in one or
 * another way .. */

#define KERNEL_HAS_MSLEEP

/* Some other kernels < 2.6.8 do not have struct kvec,
 * others do.. */

#define KERNEL_HAS_KVEC

/* Actually availabe since 2.6.26, but vendors have backported...
 */
#define KERNEL_HAS_PROC_CREATE_DATA

/* In 2.6.32 we finally fixed connector to pass netlink_skb_parms to the callback
 */
#define KERNEL_HAS_CN_SKB_PARMS
/* 2.6.39 converts connector to be syncronous, and removes .eff_cap from the
 *  * parameters. We then need to test on current_cap() instead. */
#define HAVE_NL_SKB_EFF_CAP

/* In the 2.6.34 mergewindow blk_queue_max_sectors() got blk_queue_max_hw_sectors() and
   blk_queue_max_(phys|hw)_segments() got blk_queue_max_segments()
   See Linux commits: 086fa5ff0854c676ec333 8a78362c4eefc1deddbef */
//#define NEED_BLK_QUEUE_MAX_HW_SECTORS
//#define NEED_BLK_QUEUE_MAX_SEGMENTS

/* For kernel versions 2.6.31 to 2.6.33 inclusive, even though
 * blk_queue_max_hw_sectors is present, we actually need to use
 * blk_queue_max_sectors to set max_hw_sectors. :-(
 * RHEL6 2.6.32 chose to be different and already has eliminated
 * blk_queue_max_sectors as upstream 2.6.34 did.
 * I check it into the git repo as defined,
 * because if someone does not run our compat adjust magic, it otherwise would
 * silently compile broken code on affected kernel versions, which is worse
 * than the compile error it may cause on more recent kernels.
 */
#define USE_BLK_QUEUE_MAX_SECTORS_ANYWAYS

/* For kernel versions > 2.6.38, open_bdev_excl has been replaced with
 * blkdev_get_by_path. See e525fd89 and d4d77629 */
//#define COMPAT_HAVE_BLKDEV_GET_BY_PATH

/* before open_bdev_exclusive, there was a open_bdev_excl,
 * see 30c40d2 */
#define COMPAT_HAVE_OPEN_BDEV_EXCLUSIVE

/* some old kernels do not have atomic_add_unless() */
//#define NEED_ATOMIC_ADD_UNLESS

/* some old kernels do not have the bool type */
//#define NEED_BOOL_TYPE

/* some older kernels do not have schedule_timeout_interruptible() */
//#define NEED_SCHEDULE_TIMEOUT_INTERR

/* Stone old kernels lack the fmode_t type */
#define COMPAT_HAVE_FMODE_T

/* In commit c4945b9e (v2.6.39-rc1), the little-endian bit ops got renamed */
#define COMPAT_HAVE_FIND_NEXT_ZERO_BIT_LE

/* In ancient kernels (2.6.5) kref_put() only takes a kref as argument */
//#define COMPAT_KREF_PUT_HAS_SINGLE_ARG

/* in Commit 5a7bbad27a410350e64a2d7f5ec18fc73836c14f (between Linux-3.1 and 3.2)
   make_request() becomes type void. Before it had type int. */
#define COMPAT_HAVE_VOID_MAKE_REQUEST

/* mempool_create_page_pool did not exist prior to 2.6.16 */
#define COMPAT_HAVE_MEMPOOL_CREATE_PAGE_POOL

/* bioset_create did change its signature a few times */
#define COMPAT_HAVE_BIOSET_CREATE
#define COMPAT_HAVE_BIOSET_CREATE_FRONT_PAD
//#define COMPAT_BIOSET_CREATE_HAS_THREE_PARAMETERS

/* Was added with 2.6.37 */
//#define COMPAT_HAVE_VZALLOC

/* Was added with 2.6.35 */
#define COMPAT_HAVE_UMH_WAIT_PROC

#define COMPAT_KMAP_ATOMIC_HAS_ONE_PARAMETER
//#define COMPAT_HAVE_KM_TYPE
//#define COMPAT_BIO_HAS_BI_DESTRUCTOR

#endif
