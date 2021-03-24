// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (c) 2021 Facebook */
#include <argp.h>
#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/btf.h>
#include <sys/utsname.h>
#include "retsnoop.h"
#include "retsnoop.skel.h"
#include "ksyms.h"
#include "addr2line.h"
#include "mass_attacher.h"

#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(arr[0]))

struct ctx {
	struct mass_attacher *att;
	struct retsnoop_bpf *skel;
	struct ksyms *ksyms;
	struct addr2line *a2l;
};

static struct env {
	bool verbose;
	bool debug;
	bool debug_extra;
	bool symb_lines;
	bool symb_inlines;
	const char *vmlinux_path;

	const struct preset **presets;
	char **allow_globs;
	char **deny_globs;
	char **entry_globs;
	int preset_cnt;
	int allow_glob_cnt;
	int deny_glob_cnt;
	int entry_glob_cnt;

	struct ctx ctx;
} env;

const char *argp_program_version = "retsnoop (aka dude-where-is-my-error) 0.1";
const char *argp_program_bug_address = "Andrii Nakryiko <andrii@kernel.org>";
const char argp_program_doc[] =
"retsnoop tool shows error call stacks based on specified function filters.\n"
"\n"
"USAGE: retsnoop [-v|-vv|-vvv] [-s|-ss] [-k VMLINUX_PATH] [-p PRESET]* [-a GLOB]* [-d GLOB]* [-e GLOB]*\n";

static const struct argp_option opts[] = {
	{ "verbose", 'v', "LEVEL", OPTION_ARG_OPTIONAL,
	  "Verbose output (use -vv for debug-level verbosity, -vvv for libbpf debug log)" },
	{ "preset", 'p', "PRESET", 0,
	  "Use a pre-defined set of entry/allow/deny globs for a given use case (supported presets: bpf, perf)" },
	{ "entry", 'e', "GLOB", 0,
	  "Glob for entry functions that trigger error stack trace collection" },
	{ "allow", 'a', "GLOB", 0,
	  "Glob for allowed functions captured in error stack trace collection" },
	{ "deny", 'd', "GLOB", 0,
	  "Glob for denied functions ignored during error stack trace collection" },
	{ "kernel", 'k', "PATH", 0,
	  "Path to vmlinux image with DWARF information embedded" },
	{ "symbolize", 's', "LEVEL", OPTION_ARG_OPTIONAL,
	  "Perform extra symbolization (-s gives line numbers, -ss gives also inline symbols). Relies on having vmlinux with DWARF available." },
	{},
};

struct preset {
	const char *name;
	const char **entry_globs;
	const char **allow_globs;
	const char **deny_globs;
};

static const char *bpf_entry_globs[];
static const char *bpf_allow_globs[];
static const char *bpf_deny_globs[];

static const char *perf_entry_globs[];
static const char *perf_allow_globs[];
static const char *perf_deny_globs[];

static const struct preset presets[] = {
	{"bpf", bpf_entry_globs, bpf_allow_globs, bpf_deny_globs},
	{"perf", perf_entry_globs, perf_allow_globs, perf_deny_globs},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	void *tmp, *s;
	int i;

	switch (key) {
	case 'v':
		env.verbose = true;
		if (arg) {
			if (strcmp(arg, "v") == 0) {
				env.debug = true;
			} else if (strcmp(arg, "vv") == 0) {
				env.debug = true;
				env.debug_extra = true;
			} else {
				fprintf(stderr,
					"Unrecognized verbosity setting '%s', only -v, -vv, and -vvv are supported\n",
					arg);
				return -EINVAL;
			}
		}
		break;
	case 'p':
		for (i = 0; i < ARRAY_SIZE(presets); i++) {
			const struct preset *p = &presets[i];

			if (strcmp(p->name, arg) != 0)
				continue;

			tmp = realloc(env.presets, (env.preset_cnt + 1) * sizeof(*env.presets));
			if (!tmp)
				return -ENOMEM;

			env.presets = tmp;
			env.presets[env.preset_cnt++] = p;

			return 0;
		}
		fprintf(stderr, "Unknown preset '%s' specified.\n", arg);
		break;
	case 'a':
		tmp = realloc(env.allow_globs, (env.allow_glob_cnt + 1) * sizeof(*env.allow_globs));
		if (!tmp)
			return -ENOMEM;
		s = strdup(arg);
		if (!s)
			return -ENOMEM;
		env.allow_globs = tmp;
		env.allow_globs[env.allow_glob_cnt++] = s;
		break;
	case 'd':
		tmp = realloc(env.deny_globs, (env.deny_glob_cnt + 1) * sizeof(*env.deny_globs));
		if (!tmp)
			return -ENOMEM;
		s = strdup(arg);
		if (!s)
			return -ENOMEM;
		env.deny_globs = tmp;
		env.deny_globs[env.deny_glob_cnt++] = s;
		break;
	case 'e':
		tmp = realloc(env.entry_globs, (env.entry_glob_cnt + 1) * sizeof(*env.entry_globs));
		if (!tmp)
			return -ENOMEM;
		s = strdup(arg);
		if (!s)
			return -ENOMEM;
		env.entry_globs = tmp;
		env.entry_globs[env.entry_glob_cnt++] = s;
		break;
	case 's':
		env.symb_lines = true;
		if (arg) {
			if (strcmp(arg, "s") == 0) {
				env.symb_inlines = true;
			} else {
				fprintf(stderr,
					"Unrecognized symbolization setting '%s', only -s, and -ss are supported\n",
					arg);
				return -EINVAL;
			}
		}
		break;
	case 'k':
		env.vmlinux_path = arg;
		break;
	case ARGP_KEY_ARG:
		argp_usage(state);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const struct argp argp = {
	.options = opts,
	.parser = parse_arg,
	.doc = argp_program_doc,
};

/* PRESETS */

static const char *bpf_entry_globs[] = {
	"*_sys_bpf",
	NULL,
};

static const char *bpf_allow_globs[] = {
	"*bpf_*",
	"do_check*",
	"reg_*",
	"check_*",
	"btf_*",
	"_btf_*",
	"__btf_*",
	"find_*",
	"resolve_*",
	"convert_*",
	"release_*",
	"adjust_*",
	"verifier_*",
	"verbose_*",
	"type_*",
	"arg_*",
	"sanitize_*",
	"print_*",
	"map_*",
	"ringbuf_*",
	"array_*",
	"__vmalloc_*",
	"__alloc*",
	"pcpu_*",
	"memdup_*",

	"copy_*",
	"_copy_*",
	"raw_copy_*",

	NULL,
};

static const char *bpf_deny_globs[] = {
	"bpf_get_smp_processor_id",
	"mm_init",
	"migrate_enable",
	"migrate_disable",
	"rcu_read_lock_strict",
	"rcu_read_unlock_strict",
	"__bpf_prog_enter",
	"__bpf_prog_exit",
	"__bpf_prog_enter_sleepable",
	"__bpf_prog_exit_sleepable",
	"__cant_migrate",
	"bpf_get_current_pid_tgid",
	"__bpf_prog_run_args",

	"__x64_sys_select",
	"__x64_sys_epoll_wait",
	"__x64_sys_ppoll",
	
	/* too noisy */
	"bpf_lsm_*",
	"check_cfs_rq_runtime",
	"find_busiest_group",
	"find_vma*",

	NULL,
};

static const char *perf_entry_globs[] = {
	"*_sys_perf_event_open",
	NULL,
};

static const char *perf_allow_globs[] = {
	"perf_*",
	NULL,
};

static const char *perf_deny_globs[] = {
	"bla",
	NULL,
};

/* fexit logical stack trace item */
struct fstack_item {
	const char *name;
	long res;
	long lat;
	bool finished;
	bool stitched;
};

static int filter_fstack(struct ctx *ctx, struct fstack_item *r, const struct call_stack *s)
{
	const struct mass_attacher_func_info *finfo;
	struct mass_attacher *att = ctx->att;
	struct retsnoop_bpf *skel = ctx->skel;
	struct fstack_item *fitem;
	const char *fname;
	int i, id, flags, cnt;

	for (i = 0, cnt = 0; i < s->max_depth; i++, cnt++) {
		id = s->func_ids[i];
		flags = skel->bss->func_flags[id];
		finfo = mass_attacher__func(att, id);
		fname = finfo->name;

		fitem = &r[cnt];
		fitem->name = fname;
		fitem->stitched = false;
		if (i >= s->depth) {
			fitem->finished = true;
			fitem->lat = s->func_lat[i];
		} else {
			fitem->lat = 0;
		}
		if (flags & FUNC_NEEDS_SIGN_EXT)
			fitem->res = (long)(int)s->func_res[i];
		else
			fitem->res = s->func_res[i];
		fitem->lat = s->func_lat[i];
	}

	/* no stitched together stack */
	if (s->max_depth + 1 != s->saved_depth)
		return cnt;

	for (i = s->saved_depth - 1; i < s->saved_max_depth; i++, cnt++) {
		id = s->saved_ids[i];
		flags = skel->bss->func_flags[id];
		finfo = mass_attacher__func(att, id);
		fname = finfo->name;

		fitem = &r[cnt];
		fitem->name = fname;
		fitem->stitched = true;
		fitem->finished = true;
		fitem->lat = s->saved_lat[i];
		if (flags & FUNC_NEEDS_SIGN_EXT)
			fitem->res = (long)(int)s->saved_res[i];
		else
			fitem->res = s->saved_res[i];
	}

	return cnt;
}

/* actual kernel stack trace item */
struct kstack_item {
	const struct ksym *ksym;
	long addr;
	bool filtered;
};

static bool is_bpf_tramp(const struct kstack_item *item)
{
	static char bpf_tramp_pfx[] = "bpf_trampoline_";

	if (!item->ksym)
		return false;

	return strncmp(item->ksym->name, bpf_tramp_pfx, sizeof(bpf_tramp_pfx) - 1) == 0
	       && isdigit(item->ksym->name[sizeof(bpf_tramp_pfx)]);
}

static bool is_bpf_prog(const struct kstack_item *item)
{
	static char bpf_prog_pfx[] = "bpf_prog_";

	if (!item->ksym)
		return false;

	return strncmp(item->ksym->name, bpf_prog_pfx, sizeof(bpf_prog_pfx) - 1) == 0
	       && isxdigit(item->ksym->name[sizeof(bpf_prog_pfx)]);
}

#define FTRACE_OFFSET 0x5

static int filter_kstack(struct ctx *ctx, struct kstack_item *r, const struct call_stack *s)
{
	struct ksyms *ksyms = ctx->ksyms;
	int i, n, p;

	/* lookup ksyms and reverse stack trace to match natural call order */
	n = s->kstack_sz / 8;
	for (i = 0; i < n; i++) {
		struct kstack_item *item = &r[n - i - 1];

		item->addr = s->kstack[i];
		item->filtered = false;
		item->ksym = ksyms__map_addr(ksyms, item->addr);
		if (!item->ksym)
			continue;
	}

	/* perform addiitonal post-processing to filter out bpf_trampoline and
	 * bpf_prog symbols, fixup fexit patterns, etc
	 */
	for (i = 0, p = 0; i < n; i++) {
		struct kstack_item *item = &r[p];

		*item = r[i];

		if (!item->ksym) {
			p++;
			continue;
		}

		/* Ignore bpf_trampoline frames and fix up stack traces.
		 * When fexit program happens to be inside the stack trace,
		 * a following stack trace pattern will be apparent (taking into account inverted order of frames
		 * which we did few lines above):
		 *     ffffffff8116a3d5 bpf_map_alloc_percpu+0x5
		 *     ffffffffa16db06d bpf_trampoline_6442494949_0+0x6d
		 *     ffffffff8116a40f bpf_map_alloc_percpu+0x3f
		 * 
		 * bpf_map_alloc_percpu+0x5 is real, by it just calls into the
		 * trampoline, which them calls into original call
		 * (bpf_map_alloc_percpu+0x3f). So the last item is what
		 * really matters, everything else is just a distraction, so
		 * try to detect this and filter it out. Unless we are in
		 * verbose mode, of course, in which case we live a hint
		 * that this would be filtered out (helps with debugging
		 * overall), but otherwise is preserved.
		 */
		if (i + 2 < n && is_bpf_tramp(&r[i + 1])
		    && r[i].ksym == r[i + 2].ksym
		    && r[i].addr - r[i].ksym->addr == FTRACE_OFFSET) {
			if (env.verbose) {
				item->filtered = true;
				p++;
				continue;
			}

			/* skip two elements and process useful item */
			*item = r[i + 2];
			continue;
		}

		/* Iignore bpf_trampoline and bpf_prog in stack trace, those
		 * are most probably part of our own instrumentation, but if
		 * not, you can still see them in verbose mode.
		 * Similarly, remove bpf_get_stack_raw_tp, which seems to be
		 * always there due to call to bpf_get_stack() from BPF
		 * program.
		 */
		if (is_bpf_tramp(&r[i]) || is_bpf_prog(&r[i])
		    || strcmp(r[i].ksym->name, "bpf_get_stack_raw_tp") == 0) {
			if (env.verbose) {
				item->filtered = true;
				p++;
				continue;
			}

			if (i + 1 < n)
				*item = r[i + 1];
			continue;
		}

		p++;
	}

	return p;
}

static const char *err_to_str(long err) {
	static const char *err_names[] = {
		[1] = "EPERM", [2] = "ENOENT", [3] = "ESRCH",
		[4] = "EINTR", [5] = "EIO", [6] = "ENXIO", [7] = "E2BIG",
		[8] = "ENOEXEC", [9] = "EBADF", [10] = "ECHILD", [11] = "EAGAIN",
		[12] = "ENOMEM", [13] = "EACCES", [14] = "EFAULT", [15] = "ENOTBLK",
		[16] = "EBUSY", [17] = "EEXIST", [18] = "EXDEV", [19] = "ENODEV",
		[20] = "ENOTDIR", [21] = "EISDIR", [22] = "EINVAL", [23] = "ENFILE",
		[24] = "EMFILE", [25] = "ENOTTY", [26] = "ETXTBSY", [27] = "EFBIG",
		[28] = "ENOSPC", [29] = "ESPIPE", [30] = "EROFS", [31] = "EMLINK",
		[32] = "EPIPE", [33] = "EDOM", [34] = "ERANGE", [35] = "EDEADLK",
		[36] = "ENAMETOOLONG", [37] = "ENOLCK", [38] = "ENOSYS", [39] = "ENOTEMPTY",
		[40] = "ELOOP", [42] = "ENOMSG", [43] = "EIDRM", [44] = "ECHRNG",
		[45] = "EL2NSYNC", [46] = "EL3HLT", [47] = "EL3RST", [48] = "ELNRNG",
		[49] = "EUNATCH", [50] = "ENOCSI", [51] = "EL2HLT", [52] = "EBADE",
		[53] = "EBADR", [54] = "EXFULL", [55] = "ENOANO", [56] = "EBADRQC",
		[57] = "EBADSLT", [59] = "EBFONT", [60] = "ENOSTR", [61] = "ENODATA",
		[62] = "ETIME", [63] = "ENOSR", [64] = "ENONET", [65] = "ENOPKG",
		[66] = "EREMOTE", [67] = "ENOLINK", [68] = "EADV", [69] = "ESRMNT",
		[70] = "ECOMM", [71] = "EPROTO", [72] = "EMULTIHOP", [73] = "EDOTDOT",
		[74] = "EBADMSG", [75] = "EOVERFLOW", [76] = "ENOTUNIQ", [77] = "EBADFD",
		[78] = "EREMCHG", [79] = "ELIBACC", [80] = "ELIBBAD", [81] = "ELIBSCN",
		[82] = "ELIBMAX", [83] = "ELIBEXEC", [84] = "EILSEQ", [85] = "ERESTART",
		[86] = "ESTRPIPE", [87] = "EUSERS", [88] = "ENOTSOCK", [89] = "EDESTADDRREQ",
		[90] = "EMSGSIZE", [91] = "EPROTOTYPE", [92] = "ENOPROTOOPT", [93] = "EPROTONOSUPPORT",
		[94] = "ESOCKTNOSUPPORT", [95] = "EOPNOTSUPP", [96] = "EPFNOSUPPORT", [97] = "EAFNOSUPPORT",
		[98] = "EADDRINUSE", [99] = "EADDRNOTAVAIL", [100] = "ENETDOWN", [101] = "ENETUNREACH",
		[102] = "ENETRESET", [103] = "ECONNABORTED", [104] = "ECONNRESET", [105] = "ENOBUFS",
		[106] = "EISCONN", [107] = "ENOTCONN", [108] = "ESHUTDOWN", [109] = "ETOOMANYREFS",
		[110] = "ETIMEDOUT", [111] = "ECONNREFUSED", [112] = "EHOSTDOWN", [113] = "EHOSTUNREACH",
		[114] = "EALREADY", [115] = "EINPROGRESS", [116] = "ESTALE", [117] = "EUCLEAN",
		[118] = "ENOTNAM", [119] = "ENAVAIL", [120] = "EISNAM", [121] = "EREMOTEIO",
		[122] = "EDQUOT", [123] = "ENOMEDIUM", [124] = "EMEDIUMTYPE", [125] = "ECANCELED",
		[126] = "ENOKEY", [127] = "EKEYEXPIRED", [128] = "EKEYREVOKED", [129] = "EKEYREJECTED",
		[130] = "EOWNERDEAD", [131] = "ENOTRECOVERABLE", [132] = "ERFKILL", [133] = "EHWPOISON",
		[512] = "ERESTARTSYS", [513] = "ERESTARTNOINTR", [514] = "ERESTARTNOHAND", [515] = "ENOIOCTLCMD",
		[516] = "ERESTART_RESTARTBLOCK", [517] = "EPROBE_DEFER", [518] = "EOPENSTALE", [519] = "ENOPARAM",
		[521] = "EBADHANDLE", [522] = "ENOTSYNC", [523] = "EBADCOOKIE", [524] = "ENOTSUPP",
		[525] = "ETOOSMALL", [526] = "ESERVERFAULT", [527] = "EBADTYPE", [528] = "EJUKEBOX",
		[529] = "EIOCBQUEUED", [530] = "ERECALLCONFLICT",
	};

	if (err < 0)
		err = -err;
	if (err < ARRAY_SIZE(err_names))
		return err_names[err];
	return NULL;
}

static int detect_linux_src_loc(const char *path)
{
	static const char *linux_dirs[] = {
		"arch/", "kernel/", "include/", "block/", "fs/", "net/",
		"drivers/", "mm/", "ipc/", "security/", "lib/", "crypto/",
		"certs/", "init/", "lib/", "scripts/", "sound/", "tools/",
		"usr/", "virt/", 
	};
	int i;
	char *p;

	for (i = 0; i < ARRAY_SIZE(linux_dirs); i++) {
		p = strstr(path, linux_dirs[i]);
		if (p)
			return p - path;
	}

	return 0;
}

static void print_item(struct ctx *ctx, const struct fstack_item *fitem, const struct kstack_item *kitem)
{
	const int err_width = 12;
	const int lat_width = 12;
	static struct a2l_resp resps[64];
	struct a2l_resp *resp = NULL;
	int symb_cnt = 0, i, line_off, p = 0;
	const char *fname;
	int src_print_off = 70, func_print_off;

	if (ctx->a2l && kitem && !kitem->filtered) {
		symb_cnt = addr2line__symbolize(ctx->a2l, kitem->addr, resps);
		if (symb_cnt < 0)
			symb_cnt = 0;
		if (symb_cnt > 0)
			resp = &resps[symb_cnt - 1];
	}

	/* this should be rare, either a bug or we couldn't get valid kernel
	 * stack trace
	 */
	if (!kitem)
		p += printf("!");
	else
		p += printf(" ");

	p += printf("%c ", (fitem && fitem->stitched) ? '*' : ' ');

	if (fitem && !fitem->finished) {
		p += printf("%*s %-*s ", lat_width, "...", err_width, "[...]");
	} else if (fitem) {
		p += printf("%*ldus ", lat_width - 2 /* for "us" */, fitem->lat / 1000);
		if (fitem->res == 0) {
			p += printf("%-*s ", err_width, "[NULL]");
		} else {
			const char *errstr;
			int print_cnt;

			errstr = err_to_str(fitem->res);
			if (errstr)
				print_cnt = printf("[-%s]", errstr);
			else
				print_cnt = printf("[%ld]", fitem->res);
			p += print_cnt;
			p += printf("%*s ", err_width - print_cnt, "");
		}
	} else {
		p += printf("%*s ", lat_width + 1 + err_width, "");
	}

	if (env.verbose) {
		if (kitem && kitem->filtered) 
			p += printf("~%016lx ", kitem->addr);
		else if (kitem)
			p += printf(" %016lx ", kitem->addr);
		else
			p += printf(" %*s ", 16, "");
	}

	if (kitem && kitem->ksym)
		fname = kitem->ksym->name;
	else if (fitem)
		fname = fitem->name;
	else
		fname = "";

	func_print_off = p;
	p += printf("%s", fname);
	if (kitem && kitem->ksym)
		p += printf("+0x%lx", kitem->addr - kitem->ksym->addr);
	if (symb_cnt) {
		if (env.verbose)
			src_print_off += 18; /* for extra " %16lx " */
		p += printf(" %*s(", p < src_print_off ? src_print_off - p : 0, "");

		if (strcmp(fname, resp->fname) != 0)
			p += printf("%s @ ", resp->fname);

		line_off = detect_linux_src_loc(resp->line);
		p += printf("%s)", resp->line + line_off);
	}

	p += printf("\n");

	for (i = 1, resp--; i < symb_cnt; i++, resp--) {
		p = printf("%*s. %s", func_print_off, "", resp->fname);
		line_off = detect_linux_src_loc(resp->line);
		printf(" %*s(%s)\n",
		       p < src_print_off ? src_print_off - p : 0, "",
		       resp->line + line_off);
	}
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	static struct fstack_item fstack[MAX_FSTACK_DEPTH];
	static struct kstack_item kstack[MAX_KSTACK_DEPTH];
	const struct fstack_item *fitem;
	const struct kstack_item *kitem;
	struct ctx *dctx = ctx;
	const struct call_stack *s = data;
	int i, j, fstack_n, kstack_n;

	if (!s->is_err)
		return 0;

	if (env.debug) {
		printf("GOT %s STACK (depth %u):\n", s->is_err ? "ERROR" : "SUCCESS", s->max_depth);
		printf("DEPTH %d MAX DEPTH %d SAVED DEPTH %d MAX SAVED DEPTH %d\n",
				s->depth, s->max_depth, s->saved_depth, s->saved_max_depth);
	}

	fstack_n = filter_fstack(dctx, fstack, s);
	if (fstack_n < 0) {
		fprintf(stderr, "FAILURE DURING FILTERING FUNCTION STACK!!! %d\n", fstack_n);
		return -1;
	}
	kstack_n = filter_kstack(dctx, kstack, s);
	if (kstack_n < 0) {
		fprintf(stderr, "FAILURE DURING FILTERING KERNEL STACK!!! %d\n", kstack_n);
		return -1;
	}
	if (env.debug) {
		printf("FSTACK (%d items):\n", fstack_n);
		printf("KSTACK (%d items out of original %ld):\n", kstack_n, s->kstack_sz / 8);
	}

	i = 0;
	j = 0;
	while (i < fstack_n) {
		fitem = &fstack[i];
		kitem = j < kstack_n ? &kstack[j] : NULL;

		if (!kitem) {
			/* this shouldn't happen unless we got no kernel stack
			 * or there is some bug
			 */
			print_item(dctx, fitem, NULL);
			i++;
			continue;
		}

		/* exhaust unknown kernel stack items, assuming we should find
		 * kstack_item matching current fstack_item eventually, which
		 * should be the case when kernel stack trace is correct
		 */
		if (!kitem->ksym || kitem->filtered
		    || strcmp(kitem->ksym->name, fitem->name) != 0) {
			print_item(dctx, NULL, kitem);
			j++;
			continue;
		}

		/* happy case, lots of info, yay */
		print_item(dctx, fitem, kitem);
		i++;
		j++;
		continue;
	}

	for (; j < kstack_n; j++) {
		print_item(dctx, NULL, &kstack[j]);
	}

	printf("\n\n");

	return 0;
}

static int func_flags(const char *func_name, const struct btf *btf, const struct btf_type *t)
{
	t = btf__type_by_id(btf, t->type);
	if (!t->type)
		return FUNC_CANT_FAIL;

	t = btf__type_by_id(btf, t->type);
	while (btf_is_mod(t) || btf_is_typedef(t))
		t = btf__type_by_id(btf, t->type);

	if (btf_is_ptr(t))
		return FUNC_RET_PTR; /* can fail, no sign extension */

	/* unsigned is treated as non-failing */
	if (btf_is_int(t) && !(btf_int_encoding(t) & BTF_INT_SIGNED))
		return FUNC_CANT_FAIL;

	/* byte and word are treated as non-failing */
	if (t->size < 4)
		return FUNC_CANT_FAIL;

	/* integers need sign extension */
	if (t->size == 4)
		return FUNC_NEEDS_SIGN_EXT;

	return 0;
}

static bool func_filter(const struct mass_attacher *att,
			const struct btf *btf, int func_btf_id,
			const char *name, int func_id)
{
	/* no extra filtering for now */
	return true;
}

static int find_vmlinux(char *path, size_t max_len)
{
	const char *locations[] = {
		"/boot/vmlinux-%1$s",
		"/lib/modules/%1$s/vmlinux-%1$s",
		"/lib/modules/%1$s/build/vmlinux",
		"/usr/lib/modules/%1$s/kernel/vmlinux",
		"/usr/lib/debug/boot/vmlinux-%1$s",
		"/usr/lib/debug/boot/vmlinux-%1$s.debug",
		"/usr/lib/debug/lib/modules/%1$s/vmlinux",
	};
	struct utsname buf;
	int i;

	uname(&buf);

	for (i = 0; i < ARRAY_SIZE(locations); i++) {
		snprintf(path, PATH_MAX, locations[i], buf.release);

		if (access(path, R_OK)) {
			if (env.debug)
				printf("No vmlinux image at %s found...\n", path);
			continue;
		}

		if (env.verbose)
			printf("Using vmlinux image at %s.\n", path);

		return 0;
	}

	fprintf(stderr, "Failed to locate vmlinux image location. Please use -k <vmlinux-path> to specify explicitly.\n");

	return -ESRCH;
}


static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !env.debug)
		return 0;
	return vfprintf(stderr, format, args);
}

static volatile sig_atomic_t exiting;

static void sig_handler(int sig)
{
	exiting = true;
}

int main(int argc, char **argv)
{
	struct ring_buffer *rb = NULL;
	struct mass_attacher_opts att_opts = {};
	struct mass_attacher *att = NULL;
	struct retsnoop_bpf *skel = NULL;
	const struct btf *vmlinux_btf = NULL;
	int err, i, j, k, n;

	/* Parse command line arguments */
	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return -1;

	if (env.entry_glob_cnt + env.preset_cnt == 0) {
		fprintf(stderr, "No entry point globs specified. "
				"Please provide entry glob(s) ('-e GLOB') and/or any preset ('-p PRESET').\n");
		return -1;
	}

	if (env.symb_lines) {
		char vmlinux_path[1024];

		if (!env.vmlinux_path && find_vmlinux(vmlinux_path, sizeof(vmlinux_path)))
			return -1;

		env.ctx.a2l = addr2line__init(env.vmlinux_path ?: vmlinux_path, env.symb_inlines);
		if (!env.ctx.a2l) {
			fprintf(stderr, "Failed to start addr2line for vmlinux image at %s!\n",
				env.vmlinux_path ?: vmlinux_path);
			return -1;
		}
	}

	/* Set up libbpf errors and debug info callback */
	libbpf_set_print(libbpf_print_fn);

	att_opts.verbose = env.verbose;
	att_opts.debug = env.debug;
	att_opts.debug_extra = env.debug_extra;
	att_opts.func_filter = func_filter;
	att = mass_attacher__new(&att_opts);
	if (!att)
		goto cleanup;

	for (i = 0; i < env.preset_cnt; i++) {
		const struct preset *p = env.presets[i];

		/* entry globs are also allow globs */
		for (j = 0; p->entry_globs[j]; j++) {
			err = mass_attacher__allow_glob(att, p->entry_globs[j]);
			if (err)
				goto cleanup;
		}
		for (j = 0; p->allow_globs[j]; j++) {
			err = mass_attacher__allow_glob(att, p->allow_globs[j]);
			if (err)
				goto cleanup;
		}
		for (j = 0; p->deny_globs[j]; j++) {
			err = mass_attacher__deny_glob(att, p->deny_globs[j]);
			if (err)
				goto cleanup;
		}
	}
	/* entry globs are allow globs as well */
	for (i = 0; i < env.entry_glob_cnt; i++) {
		err = mass_attacher__allow_glob(att, env.entry_globs[i]);
		if (err)
			goto cleanup;
	}
	for (i = 0; i < env.allow_glob_cnt; i++) {
		err = mass_attacher__allow_glob(att, env.allow_globs[i]);
		if (err)
			goto cleanup;
	}
	for (i = 0; i < env.deny_glob_cnt; i++) {
		err = mass_attacher__deny_glob(att, env.deny_globs[i]);
		if (err)
			goto cleanup;
	}

	err = mass_attacher__prepare(att);
	if (err)
		goto cleanup;

	skel = mass_attacher__skeleton(att);
	if (env.verbose)
		skel->rodata->verbose = true;
	
	vmlinux_btf = mass_attacher__btf(att);
	for (i = 0, n = mass_attacher__func_cnt(att); i < n; i++) {
		const struct mass_attacher_func_info *finfo;
		const struct btf_type *t;
		const char *glob;
		__u32 flags;

		finfo = mass_attacher__func(att, i);
		t = btf__type_by_id(vmlinux_btf, finfo->btf_id);
		flags = func_flags(finfo->name, vmlinux_btf, t);

		for (j = 0; j < env.entry_glob_cnt; j++) {
			glob = env.entry_globs[j];
			if (!glob_matches(glob, finfo->name))
				continue;

			flags |= FUNC_IS_ENTRY;

			if (env.verbose)
				printf("Function '%s' is marked as an entry point.\n", finfo->name);
			goto done;
		}
		for (j = 0; j < env.preset_cnt; j++) {
			for (k = 0; env.presets[j]->entry_globs[k]; k++) {
				glob = env.presets[j]->entry_globs[k];
				if (!glob_matches(glob, finfo->name))
					continue;

				flags |= FUNC_IS_ENTRY;

				if (env.verbose)
					printf("Function '%s' is marked as an entry point.\n", finfo->name);
				goto done;
			}
		}
done:
		strcpy(skel->bss->func_names[i], finfo->name);
		skel->bss->func_ips[i] = finfo->addr;
		skel->bss->func_flags[i] = flags;
	}

	err = mass_attacher__load(att);
	if (err)
		goto cleanup;

	err = mass_attacher__attach(att);
	if (err)
		goto cleanup;

	signal(SIGINT, sig_handler);

	env.ctx.att = att;
	env.ctx.skel = mass_attacher__skeleton(att);
	env.ctx.ksyms = ksyms__load();
	if (!env.ctx.ksyms) {
		fprintf(stderr, "Failed to load /proc/kallsyms for symbolization.\n");
		goto cleanup;
	}


	/* Set up ring buffer polling */
	rb = ring_buffer__new(bpf_map__fd(env.ctx.skel->maps.rb), handle_event, &env.ctx, NULL);
	if (!rb) {
		err = -1;
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	/* Allow mass tracing */
	mass_attacher__activate(att);

	/* Process events */
	printf("Receiving data...\n");
	while (!exiting) {
		err = ring_buffer__poll(rb, 100 /* timeout, ms */);
		/* Ctrl-C will cause -EINTR */
		if (err == -EINTR) {
			err = 0;
			goto cleanup;
		}
		if (err < 0) {
			printf("Error polling perf buffer: %d\n", err);
			goto cleanup;
		}
	}

cleanup:
	printf("Detaching, be patient...\n");
	mass_attacher__free(att);

	addr2line__free(env.ctx.a2l);
	ksyms__free(env.ctx.ksyms);

	for (i = 0; i < env.allow_glob_cnt; i++)
		free(env.allow_globs[i]);
	free(env.allow_globs);
	for (i = 0; i < env.deny_glob_cnt; i++)
		free(env.deny_globs[i]);
	free(env.deny_globs);
	for (i = 0; i < env.entry_glob_cnt; i++)
		free(env.entry_globs[i]);
	free(env.entry_globs);
	free(env.presets);

	return -err;
}