/*
 * Bionic gap-fillers for BusyBox, force-included into every translation unit
 * through CONFIG_EXTRA_CFLAGS (-include).
 *
 * Everything here exists because bionic genuinely lacks the function at the API
 * level this app targets, and because the BusyBox code that calls it is NOT
 * behind a config guard, so disabling a feature would not avoid it and the
 * only alternatives are dropping a useful applet or supplying the function.
 *
 * Rule for anything added here: it must be a faithful implementation of the
 * documented behaviour, not a stub that returns success. A stub that lies is
 * worse than a missing applet, because the failure surfaces later and somewhere
 * else.
 */
#ifndef DRACXTERM_BUSYBOX_BIONIC_COMPAT_H
#define DRACXTERM_BUSYBOX_BIONIC_COMPAT_H

#if defined(__ANDROID__)

#include <stdio.h>
#include <mntent.h>

/*
 * addmntent(3). bionic declares setmntent/getmntent/getmntent_r/endmntent but
 * not addmntent, at any API level (checked against NDK r27 sysroot).
 *
 * util-linux/mount.c calls it at the end of singlemount() to append the entry
 * it just mounted to mtab. The call sits outside the ENABLE_FEATURE_MTAB_SUPPORT
 * guard, so it is compiled even with mtab support switched off.
 *
 * Format is the one fstab(5) specifies and getmntent parses back: six
 * whitespace-separated fields, newline-terminated. Returns 0 on success and 1
 * on failure, matching glibc.
 */
__attribute__((unused))
static int addmntent(FILE *stream, const struct mntent *mnt)
{
	if (stream == NULL || mnt == NULL)
		return 1;
	if (fseek(stream, 0, SEEK_END) != 0)
		return 1;
	if (fprintf(stream, "%s %s %s %s %d %d\n",
		    mnt->mnt_fsname ? mnt->mnt_fsname : "none",
		    mnt->mnt_dir    ? mnt->mnt_dir    : "none",
		    mnt->mnt_type   ? mnt->mnt_type   : "none",
		    mnt->mnt_opts   ? mnt->mnt_opts   : "defaults",
		    mnt->mnt_freq,
		    mnt->mnt_passno) < 0)
		return 1;
	return 0;
}

#endif /* __ANDROID__ */
#endif /* DRACXTERM_BUSYBOX_BIONIC_COMPAT_H */
