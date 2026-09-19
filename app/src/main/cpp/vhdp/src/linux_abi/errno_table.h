/* Linux errno names and short descriptions, independent of libc locale/strerror. */
#ifndef VHDP_LINUX_ABI_ERRNO_TABLE_H
#define VHDP_LINUX_ABI_ERRNO_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a static string such as "ENOENT"; "E?" for unknown values. */
const char* vhdp_errno_name(int err);
/* Returns a static English description; "unknown error" for unknown values. */
const char* vhdp_errno_description(int err);

#ifdef __cplusplus
}
#endif

#endif
