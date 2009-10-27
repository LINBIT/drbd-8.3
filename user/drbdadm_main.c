/*
   drbdadm_main.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2002-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 2002-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <search.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include "drbdtool_common.h"
#include "drbdadm.h"

#define MAX_ARGS 40

static int indent = 0;
#define INDENT_WIDTH 4
#define BFMT  "%s;\n"
#define IPV4FMT "%-16s %s %s:%s;\n"
#define IPV6FMT "%-16s %s [%s]:%s;\n"
#define MDISK "%-16s %s [%s];\n"
#define FMDISK "%-16s %s;\n"
#define printI(fmt, args... ) printf("%*s" fmt,INDENT_WIDTH * indent,"" , ## args )
#define printA(name, val ) \
	printf("%*s%*s %3s;\n", \
	  INDENT_WIDTH * indent,"" , \
	  -24+INDENT_WIDTH * indent, \
	  name, val )

char *progname;

struct adm_cmd {
	const char *name;
	int (*function) (struct d_resource *, const char *);
	/* which level this command is for.
	 * 0: don't show this command, ever
	 * 1: normal administrative commands, shown in normal help
	 * 2-4: shown on "drbdadm hidden-commands"
	 * 2: useful for shell scripts
	 * 3: callbacks potentially called from kernel module on certain events
	 * 4: advanced, experts and developers only */
	unsigned int show_in_usage:3;
	/* if set, command requires an explicit resource name */
	unsigned int res_name_required:1;
	/* error out if the ip specified is not available/active now */
	unsigned int verify_ips:1;
	/* if set, use the "cache" in /var/lib/drbd to figure out
	 * which config file to use.
	 * This is necessary for handlers (callbacks from kernel) to work
	 * when using "drbdadm -c /some/other/config/file" */
	unsigned int use_cached_config_file:1;
	unsigned int need_peer:1;
	unsigned int is_proxy_cmd:1;
	unsigned int uc_dialog:1; /* May show usage count dialog */
};

struct deferred_cmd {
	int (*function) (struct d_resource *, const char *);
	const char *arg;
	struct d_resource *res;
	struct deferred_cmd *next;
};

extern int my_parse();
extern int yydebug;
extern FILE *yyin;

int adm_attach(struct d_resource *, const char *);
int adm_connect(struct d_resource *, const char *);
int adm_generic_s(struct d_resource *, const char *);
int adm_status_xml(struct d_resource *, const char *);
int adm_generic_l(struct d_resource *, const char *);
int adm_resize(struct d_resource *, const char *);
int adm_syncer(struct d_resource *, const char *);
static int adm_up(struct d_resource *, const char *);
extern int adm_adjust(struct d_resource *, const char *);
static int adm_dump(struct d_resource *, const char *);
static int adm_dump_xml(struct d_resource *, const char *);
static int adm_wait_c(struct d_resource *, const char *);
static int adm_wait_ci(struct d_resource *, const char *);
static int adm_proxy_up(struct d_resource *, const char *);
static int adm_proxy_down(struct d_resource *, const char *);
static int sh_nop(struct d_resource *, const char *);
static int sh_resources(struct d_resource *, const char *);
static int sh_resource(struct d_resource *, const char *);
static int sh_mod_parms(struct d_resource *, const char *);
static int sh_dev(struct d_resource *, const char *);
static int sh_udev(struct d_resource *, const char *);
static int sh_minor(struct d_resource *, const char *);
static int sh_ip(struct d_resource *, const char *);
static int sh_lres(struct d_resource *, const char *);
static int sh_ll_dev(struct d_resource *, const char *);
static int sh_md_dev(struct d_resource *, const char *);
static int sh_md_idx(struct d_resource *, const char *);
static int sh_b_pri(struct d_resource *, const char *);
static int sh_status(struct d_resource *, const char *);
static int admm_generic(struct d_resource *, const char *);
static int adm_khelper(struct d_resource *, const char *);
static int adm_generic_b(struct d_resource *, const char *);
static int hidden_cmds(struct d_resource *, const char *);
static int adm_outdate(struct d_resource *, const char *);

static char *get_opt_val(struct d_option *, const char *, char *);
static void register_config_file(struct d_resource *res, const char *cfname);

static struct ifreq *get_ifreq();

char ss_buffer[255];
struct utsname nodeinfo;
int line = 1;
int fline;
struct d_globals global_options = { 0, 0, 0, 1, UC_ASK };

char *config_file = NULL;
char *config_save = NULL;
struct d_resource *config = NULL;
struct d_resource *common = NULL;
struct ifreq *ifreq_list = NULL;
int is_drbd_top;
int nr_resources;
int nr_stacked;
int nr_normal;
int nr_ignore;
int highest_minor;
int config_from_stdin = 0;
int config_valid = 1;
int no_tty;
int dry_run = 0;
int verbose = 0;
int do_verify_ips = 0;
int do_register_minor = 1;
/* whether drbdadm was called with "all" instead of resource name(s) */
int all_resources = 0;
char *drbdsetup = NULL;
char *drbdmeta = NULL;
char *drbd_proxy_ctl;
char *sh_varname = NULL;
char *setup_opts[10];
char *connect_to_host = NULL;
int soi = 0;
volatile int alarm_raised;

struct deferred_cmd *deferred_cmds[3] = { NULL, NULL, NULL };
struct deferred_cmd *deferred_cmds_tail[3] = { NULL, NULL, NULL };

void schedule_dcmd(int (*function) (struct d_resource *, const char *),
		   struct d_resource *res, char *arg, int order)
{
	struct deferred_cmd *d, *t;

	d = calloc(1, sizeof(struct deferred_cmd));
	if (d == NULL) {
		perror("calloc");
		exit(E_exec_error);
	}

	d->function = function;
	d->res = res;
	d->arg = arg;

	/* first to come is head */
	if (!deferred_cmds[order])
		deferred_cmds[order] = d;

	/* link it in at tail */
	t = deferred_cmds_tail[order];
	if (t)
		t->next = d;

	/* advance tail */
	deferred_cmds_tail[order] = d;
}

static int adm_generic(struct d_resource *res, const char *cmd, int flags);

/* Returns non-zero if the resource is down. */
static int test_if_resource_is_down(struct d_resource *res)
{
	char buf[1024];
	char *line;
	int fd;
	int old_verbose = verbose;
	FILE *f;

	if (dry_run) {
		fprintf(stderr, "Logic bug: should not be dry-running here.\n");
		exit(E_thinko);
	}
	if (verbose == 1)
		verbose = 0;
	fd = adm_generic(res, "role", RETURN_STDOUT_FD | SUPRESS_STDERR);
	verbose = old_verbose;

	if (fd < 0) {
		fprintf(stderr, "Strange: got negative fd.\n");
		exit(E_thinko);
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		fprintf(stderr, "Bad: could not fdopen file descriptor.\n");
		exit(E_thinko);
	}

	errno = 0;
	line = fgets(buf, sizeof(buf) - 1, f);
	if (line == NULL && errno != 0) {
		fprintf(stderr,
			"Bad: fgets returned NULL while waiting for data: %m\n");
		exit(E_thinko);
	}
	fclose(f);

	waitpid(0, NULL, WNOHANG);	/* Reap the child process, do not leave a zombie around. */

	if (line == NULL
	    || strncmp(line, "Unconfigured", strlen("Unconfigured")) == 0)
		return 1;

	return 0;
}

enum do_register { SAME_ANYWAYS, DO_REGISTER };
enum do_register if_conf_differs_confirm_or_abort(struct d_resource *res)
{
	int minor = res->me->device_minor;
	char *f;

	/* if the resource was down,
	 * just register the new config file */
	if (test_if_resource_is_down(res)) {
		unregister_minor(minor);
		return DO_REGISTER;
	}

	f = lookup_minor(minor);

	/* if there was nothing registered before,
	 * there is nothing to compare to */
	if (!f)
		return DO_REGISTER;

	/* no need to register the same thing again */
	if (strcmp(f, config_save) == 0)
		return SAME_ANYWAYS;

	fprintf(stderr, "Warning: resource %s\n"
		"last used config file: %s\n"
		"  current config file: %s\n", res->name, f, config_save);

	/* implicitly force if we don't have a tty */
	if (no_tty)
		force = 1;

	if (!confirmed("Do you want to proceed "
		       "and register the current config file?")) {
		printf("Operation canceled.\n");
		exit(E_usage);
	}
	return DO_REGISTER;
}

static void register_config_file(struct d_resource *res, const char *cfname)
{
	int minor = res->me->device_minor;
	if (test_if_resource_is_down(res))
		unregister_minor(minor);
	else
		register_minor(minor, cfname);
}

enum on_error { KEEP_RUNNING, EXIT_ON_FAIL };
int call_cmd_fn(int (*function) (struct d_resource *, const char *),
		const char *fn_name, struct d_resource *res,
		enum on_error on_error)
{
	int rv;
	int really_register = do_register_minor &&
	    DO_REGISTER == if_conf_differs_confirm_or_abort(res) &&
	    /* adm_up and adm_adjust only
	     * "schedule" the commands, don't register yet! */
	    function != adm_up && function != adm_adjust;

	rv = function(res, fn_name);
	if (rv >= 20) {
		fprintf(stderr, "%s %s %s: exited with code %d\n",
			progname, fn_name, res->name, rv);
		if (on_error == EXIT_ON_FAIL)
			exit(rv);
	}
	if (rv == 0 && really_register)
		register_config_file(res, config_save);

	return rv;
}

int call_cmd(struct adm_cmd *cmd, struct d_resource *res,
	     enum on_error on_error)
{
	if (!res->peer)
		set_peer_in_resource(res, cmd->need_peer);

	return call_cmd_fn(cmd->function, cmd->name, res, on_error);
}

int _run_dcmds(int order)
{
	struct deferred_cmd *d = deferred_cmds[order];
	struct deferred_cmd *t;
	int r = 0;
	int rv = 0;

	while (d) {
		r = call_cmd_fn(d->function, d->arg, d->res, KEEP_RUNNING);
		t = d->next;
		free(d);
		d = t;
		if (r > rv)
			rv = r;
	}
	return rv;
}

int run_dcmds(void)
{
	return _run_dcmds(0) || _run_dcmds(1) || _run_dcmds(2);
}

struct option admopt[] = {
	{"stacked", no_argument, 0, 'S'},
	{"dry-run", no_argument, 0, 'd'},
	{"verbose", no_argument, 0, 'v'},
	{"config-file", required_argument, 0, 'c'},
	{"drbdsetup", required_argument, 0, 's'},
	{"drbdmeta", required_argument, 0, 'm'},
	{"drbd-proxy-ctl", required_argument, 0, 'p'},
	{"sh-varname", required_argument, 0, 'n'},
	{"force", no_argument, 0, 'f'},
	{"peer", required_argument, 0, 'P'},
	{"version", no_argument, 0, 'V'},
	{0, 0, 0, 0}
};

/* DRBD adm_cmd flags shortcuts,
 * to avoid merge conflicts and unreadable diffs
 * when we add the next flag */

#define DRBD_acf1_default		\
	.show_in_usage = 1,		\
	.res_name_required = 1,		\
	.verify_ips = 0,		\
	.uc_dialog = 1,			\

#define DRBD_acf1_connect		\
	.show_in_usage = 1,		\
	.res_name_required = 1,		\
	.verify_ips = 1,		\
	.need_peer = 1,			\
	.uc_dialog = 1,			\

#define DRBD_acf1_defnet		\
	.show_in_usage = 1,		\
	.res_name_required = 1,		\
	.verify_ips = 1,		\
	.uc_dialog = 1,			\

#define DRBD_acf3_handler		\
	.show_in_usage = 3,		\
	.res_name_required = 1,		\
	.verify_ips = 0,		\
	.use_cached_config_file = 1,	\

#define DRBD_acf4_advanced		\
	.show_in_usage = 4,		\
	.res_name_required = 1,		\
	.verify_ips = 0,		\
	.uc_dialog = 1,			\

#define DRBD_acf1_dump			\
	.show_in_usage = 1,		\
	.res_name_required = 1,		\
	.verify_ips = 1,		\
	.uc_dialog = 1,			\

#define DRBD_acf2_shell			\
	.show_in_usage = 2,		\
	.res_name_required = 1,		\
	.verify_ips = 0,		\

#define DRBD_acf2_proxy			\
	.show_in_usage = 2,		\
	.res_name_required = 1,		\
	.verify_ips = 0,		\
	.need_peer = 1,			\
	.is_proxy_cmd = 1,		\

#define DRBD_acf2_hook			\
	.show_in_usage = 2,		\
	.res_name_required = 1,		\
	.verify_ips = 0,                \
	.use_cached_config_file = 1,	\

#define DRBD_acf2_gen_shell		\
	.show_in_usage = 2,		\
	.res_name_required = 0,		\
	.verify_ips = 0,		\

struct adm_cmd cmds[] = {
	/*  name, function, flags
	 *  sort order:
	 *  - normal config commands,
	 *  - normal meta data manipulation
	 *  - sh-*
	 *  - handler
	 *  - advanced
	 ***/
	{"attach", adm_attach, DRBD_acf1_default},
	{"detach", adm_generic_l, DRBD_acf1_default},
	{"connect", adm_connect, DRBD_acf1_connect},
	{"disconnect", adm_generic_s, DRBD_acf1_default},
	{"up", adm_up, DRBD_acf1_connect},
	{"down", adm_generic_l, DRBD_acf1_default},
	{"primary", adm_generic_l, DRBD_acf1_default},
	{"secondary", adm_generic_l, DRBD_acf1_default},
	{"invalidate", adm_generic_b, DRBD_acf1_default},
	{"invalidate-remote", adm_generic_l, DRBD_acf1_defnet},
	{"outdate", adm_outdate, DRBD_acf1_default},
	{"resize", adm_resize, DRBD_acf1_defnet},
	{"syncer", adm_syncer, DRBD_acf1_defnet},
	{"verify", adm_generic_s, DRBD_acf1_defnet},
	{"pause-sync", adm_generic_s, DRBD_acf1_defnet},
	{"resume-sync", adm_generic_s, DRBD_acf1_defnet},
	{"adjust", adm_adjust, DRBD_acf1_connect},
	{"wait-connect", adm_wait_c, DRBD_acf1_defnet},
	{"wait-con-int", adm_wait_ci,
	 .show_in_usage = 1,.verify_ips = 1,},
	{"status", adm_status_xml, DRBD_acf2_gen_shell},
	{"role", adm_generic_s, DRBD_acf1_default},
	{"cstate", adm_generic_s, DRBD_acf1_default},
	{"dstate", adm_generic_b, DRBD_acf1_default},

	{"dump", adm_dump, DRBD_acf1_dump},
	{"dump-xml", adm_dump_xml, DRBD_acf1_dump},

	{"create-md", adm_create_md, DRBD_acf1_default},
	{"show-gi", adm_generic_b, DRBD_acf1_default},
	{"get-gi", adm_generic_b, DRBD_acf1_default},
	{"dump-md", admm_generic, DRBD_acf1_default},
	{"wipe-md", admm_generic, DRBD_acf1_default},
	{"hidden-commands", hidden_cmds,.show_in_usage = 1,},

	{"sh-nop", sh_nop, DRBD_acf2_gen_shell .uc_dialog = 1},
	{"sh-resources", sh_resources, DRBD_acf2_gen_shell},
	{"sh-resource", sh_resource, DRBD_acf2_shell},
	{"sh-mod-parms", sh_mod_parms, DRBD_acf2_gen_shell},
	{"sh-dev", sh_dev, DRBD_acf2_shell},
	{"sh-udev", sh_udev, DRBD_acf2_hook},
	{"sh-minor", sh_minor, DRBD_acf2_shell},
	{"sh-ll-dev", sh_ll_dev, DRBD_acf2_shell},
	{"sh-md-dev", sh_md_dev, DRBD_acf2_shell},
	{"sh-md-idx", sh_md_idx, DRBD_acf2_shell},
	{"sh-ip", sh_ip, DRBD_acf2_shell},
	{"sh-lr-of", sh_lres, DRBD_acf2_shell},
	{"sh-b-pri", sh_b_pri, DRBD_acf2_shell},
	{"sh-status", sh_status, DRBD_acf2_gen_shell},

	{"proxy-up", adm_proxy_up, DRBD_acf2_proxy},
	{"proxy-down", adm_proxy_down, DRBD_acf2_proxy},

	{"before-resync-target", adm_khelper, DRBD_acf3_handler},
	{"after-resync-target", adm_khelper, DRBD_acf3_handler},
	{"pri-on-incon-degr", adm_khelper, DRBD_acf3_handler},
	{"pri-lost-after-sb", adm_khelper, DRBD_acf3_handler},
	{"fence-peer", adm_khelper, DRBD_acf3_handler},
	{"local-io-error", adm_khelper, DRBD_acf3_handler},
	{"pri-lost", adm_khelper, DRBD_acf3_handler},
	{"split-brain", adm_khelper, DRBD_acf3_handler},
	{"out-of-sync", adm_khelper, DRBD_acf3_handler},

	{"suspend-io", adm_generic_s, DRBD_acf4_advanced},
	{"resume-io", adm_generic_s, DRBD_acf4_advanced},
	{"set-gi", admm_generic, DRBD_acf4_advanced},
	{"new-current-uuid", adm_generic_s, DRBD_acf4_advanced},
};

/*** These functions are used to the print the config ***/

static char *esc(char *str)
{
	static char buffer[1024];
	char *ue = str, *e = buffer;

	if (!str || !str[0]) {
		return "\"\"";
	}
	if (strchr(str, ' ') || strchr(str, '\t') || strchr(str, '\\')) {
		*e++ = '"';
		while (*ue) {
			if (*ue == '"' || *ue == '\\') {
				*e++ = '\\';
			}
			if (e - buffer >= 1022) {
				fprintf(stderr, "string too long.\n");
				exit(E_syntax);
			}
			*e++ = *ue++;
			if (e - buffer >= 1022) {
				fprintf(stderr, "string too long.\n");
				exit(E_syntax);
			}
		}
		*e++ = '"';
		*e++ = '\0';
		return buffer;
	}
	return str;
}

static char *esc_xml(char *str)
{
	static char buffer[1024];
	char *ue = str, *e = buffer;

	if (!str || !str[0]) {
		return "";
	}
	if (strchr(str, '"') || strchr(str, '\'') || strchr(str, '<') ||
	    strchr(str, '>') || strchr(str, '&') || strchr(str, '\\')) {
		while (*ue) {
			if (*ue == '"' || *ue == '\\') {
				*e++ = '\\';
				if (e - buffer >= 1021) {
					fprintf(stderr, "string too long.\n");
					exit(E_syntax);
				}
				*e++ = *ue++;
			} else if (*ue == '\'' || *ue == '<' || *ue == '>'
				   || *ue == '&') {
				if (*ue == '\'' && e - buffer < 1017) {
					strcpy(e, "&apos;");
					e += 6;
				} else if (*ue == '<' && e - buffer < 1019) {
					strcpy(e, "&lt;");
					e += 4;
				} else if (*ue == '>' && e - buffer < 1019) {
					strcpy(e, "&gt;");
					e += 4;
				} else if (*ue == '&' && e - buffer < 1018) {
					strcpy(e, "&amp;");
					e += 5;
				} else {
					fprintf(stderr, "string too long.\n");
					exit(E_syntax);
				}
				ue++;
			} else {
				*e++ = *ue++;
				if (e - buffer >= 1022) {
					fprintf(stderr, "string too long.\n");
					exit(E_syntax);
				}
			}
		}
		*e++ = '\0';
		return buffer;
	}
	return str;
}

static void dump_options(char *name, struct d_option *opts)
{
	if (!opts)
		return;

	printI("%s {\n", name);
	++indent;
	while (opts) {
		if (opts->value)
			printA(opts->name,
			       opts->is_escaped ? opts->value : esc(opts->
								    value));
		else
			printI(BFMT, opts->name);
		opts = opts->next;
	}
	--indent;
	printI("}\n");
}

static void dump_global_info()
{
	if (!global_options.minor_count
	    && !global_options.disable_ip_verification
	    && global_options.dialog_refresh == 1)
		return;
	printI("global {\n");
	++indent;
	if (global_options.disable_ip_verification)
		printI("disable-ip-verification;\n");
	if (global_options.minor_count)
		printI("minor-count %i;\n", global_options.minor_count);
	if (global_options.dialog_refresh != 1)
		printI("dialog-refresh %i;\n", global_options.dialog_refresh);
	--indent;
	printI("}\n\n");
}

static void fake_startup_options(struct d_resource *res);

static void dump_common_info()
{
	if (!common)
		return;
	printI("common {\n");
	++indent;
	if (common->protocol)
		printA("protocol", common->protocol);
	fake_startup_options(common);
	dump_options("net", common->net_options);
	dump_options("disk", common->disk_options);
	dump_options("syncer", common->sync_options);
	dump_options("startup", common->startup_options);
	dump_options("proxy", common->proxy_options);
	dump_options("handlers", common->handlers);
	--indent;
	printf("}\n\n");
}

static void dump_address(char *name, char *addr, char *port, char *af)
{
	if (!strcmp(af, "ipv6"))
		printI(IPV6FMT, name, af, addr, port);
	else
		printI(IPV4FMT, name, af, addr, port);
}

static void dump_proxy_info(struct d_proxy_info *pi)
{
	printI("proxy on %s {\n", names_to_str(pi->on_hosts));
	++indent;
	dump_address("inside", pi->inside_addr, pi->inside_port, pi->inside_af);
	dump_address("outside", pi->outside_addr, pi->outside_port,
		     pi->outside_af);
	--indent;
	printI("}\n");
}

static void dump_host_info(struct d_host_info *hi)
{
	if (!hi) {
		printI("  # No host section data available.\n");
		return;
	}

	if (hi->lower) {
		printI("stacked-on-top-of %s {\n", esc(hi->lower->name));
		++indent;
		printI("# on %s \n", names_to_str(hi->on_hosts));
	} else if (hi->by_address) {
		if (!strcmp(hi->address_family, "ipv6"))
			printI("floating ipv6 [%s]:%s {\n", hi->address, hi->port);
		else
			printI("floating %s %s:%s {\n", hi->address_family, hi->address, hi->port);
		++indent;
	} else {
		printI("on %s {\n", names_to_str(hi->on_hosts));
		++indent;
	}
	printI("device%*s", -19 + INDENT_WIDTH * indent, "");
	if (hi->device)
		printf("%s ", esc(hi->device));
	printf("minor %d;\n", hi->device_minor);
	if (!hi->lower)
		printA("disk", esc(hi->disk));
	if (!hi->by_address)
		dump_address("address", hi->address, hi->port, hi->address_family);
	if (!hi->lower) {
		if (!strncmp(hi->meta_index, "flex", 4))
			printI(FMDISK, "flexible-meta-disk",
			       esc(hi->meta_disk));
		else if (!strcmp(hi->meta_index, "internal"))
			printA("meta-disk", "internal");
		else
			printI(MDISK, "meta-disk", esc(hi->meta_disk),
			       hi->meta_index);
	}
	if (hi->proxy)
		dump_proxy_info(hi->proxy);
	--indent;
	printI("}\n");
}

static void dump_options_xml(char *name, struct d_option *opts)
{
	if (!opts)
		return;

	printI("<section name=\"%s\">\n", name);
	++indent;
	while (opts) {
		if (opts->value)
			printI("<option name=\"%s\" value=\"%s\"/>\n",
			       opts->name,
			       opts->is_escaped ? opts->value : esc_xml(opts->
									value));
		else
			printI("<option name=\"%s\"/>\n", opts->name);
		opts = opts->next;
	}
	--indent;
	printI("</section>\n");
}

static void dump_global_info_xml()
{
	if (!global_options.minor_count
	    && !global_options.disable_ip_verification
	    && global_options.dialog_refresh == 1)
		return;
	printI("<global>\n");
	++indent;
	if (global_options.disable_ip_verification)
		printI("<disable-ip-verification/>\n");
	if (global_options.minor_count)
		printI("<minor-count count=\"%i\"/>\n",
		       global_options.minor_count);
	if (global_options.dialog_refresh != 1)
		printI("<dialog-refresh refresh=\"%i\"/>\n",
		       global_options.dialog_refresh);
	--indent;
	printI("</global>\n");
}

static void dump_common_info_xml()
{
	if (!common)
		return;
	printI("<common");
	if (common->protocol)
		printf(" protocol=\"%s\"", common->protocol);
	printf(">\n");
	++indent;
	fake_startup_options(common);
	dump_options_xml("net", common->net_options);
	dump_options_xml("disk", common->disk_options);
	dump_options_xml("syncer", common->sync_options);
	dump_options_xml("startup", common->startup_options);
	dump_options_xml("proxy", common->proxy_options);
	dump_options_xml("handlers", common->handlers);
	--indent;
	printI("</common>\n");
}

static void dump_proxy_info_xml(struct d_proxy_info *pi)
{
	printI("<proxy hostname=\"%s\">\n", names_to_str(pi->on_hosts));
	++indent;
	printI("<inside family=\"%s\" port=\"%s\">%s</inside>\n", pi->inside_af,
	       pi->inside_port, pi->inside_addr);
	printI("<outside family=\"%s\" port=\"%s\">%s</outside>\n",
	       pi->outside_af, pi->outside_port, pi->outside_addr);
	--indent;
	printI("</proxy>\n");
}

static void dump_host_info_xml(struct d_host_info *hi)
{
	if (!hi) {
		printI("<!-- No host section data available. -->\n");
		return;
	}

	if (hi->by_address)
		printI("<host floating=\"1\">\n");
	else
		printI("<host name=\"%s\">\n", names_to_str(hi->on_hosts));

	++indent;
	printI("<device minor=\"%d\">%s</device>\n", hi->device_minor,
	       esc_xml(hi->device));
	printI("<disk>%s</disk>\n", esc_xml(hi->disk));
	printI("<address family=\"%s\" port=\"%s\">%s</address>\n",
	       hi->address_family, hi->port, hi->address);
	if (!strncmp(hi->meta_index, "flex", 4))
		printI("<flexible-meta-disk>%s</flexible-meta-disk>\n",
		       esc_xml(hi->meta_disk));
	else if (!strcmp(hi->meta_index, "internal"))
		printI("<meta-disk>internal</meta-disk>\n");
	else {
		printI("<meta-disk index=\"%s\">%s</meta-disk>\n",
		       hi->meta_index, esc_xml(hi->meta_disk));
	}
	if (hi->proxy)
		dump_proxy_info_xml(hi->proxy);
	--indent;
	printI("</host>\n");
}

static void fake_startup_options(struct d_resource *res)
{
	struct d_option *opt;
	char *val;

	if (res->stacked_timeouts) {
		opt = new_opt(strdup("stacked-timeouts"), NULL);
		res->startup_options = APPEND(res->startup_options, opt);
	}

	if (res->become_primary_on) {
		val = strdup(names_to_str(res->become_primary_on));
		opt = new_opt(strdup("become-primary-on"), val);
		opt->is_escaped = 1;
		res->startup_options = APPEND(res->startup_options, opt);
	}
}

static int adm_dump(struct d_resource *res,
		    const char *unused __attribute((unused)))
{
	struct d_host_info *host;

	printI("# resource %s on %s: %s, %s\n",
	       esc(res->name), nodeinfo.nodename,
	       res->ignore ? "ignored" : "not ignored",
	       res->stacked ? "stacked" : "not stacked");
	printI("resource %s {\n", esc(res->name));
	++indent;
	if (res->protocol)
		printA("protocol", res->protocol);

	for (host = res->all_hosts; host; host = host->next)
		dump_host_info(host);

	fake_startup_options(res);
	dump_options("net", res->net_options);
	dump_options("disk", res->disk_options);
	dump_options("syncer", res->sync_options);
	dump_options("startup", res->startup_options);
	dump_options("proxy", res->proxy_options);
	dump_options("handlers", res->handlers);
	--indent;
	printf("}\n\n");

	return 0;
}

static int adm_dump_xml(struct d_resource *res,
			const char *unused __attribute((unused)))
{
	struct d_host_info *host;
	printI("<resource name=\"%s\"", esc_xml(res->name));
	if (res->protocol)
		printf(" protocol=\"%s\"", res->protocol);
	printf(">\n");
	++indent;
	// else if (common && common->protocol) printA("# common protocol", common->protocol);
	for (host = res->all_hosts; host; host = host->next)
		dump_host_info_xml(host);
	fake_startup_options(res);
	dump_options_xml("net", res->net_options);
	dump_options_xml("disk", res->disk_options);
	dump_options_xml("syncer", res->sync_options);
	dump_options_xml("startup", res->startup_options);
	dump_options_xml("proxy", res->proxy_options);
	dump_options_xml("handlers", res->handlers);
	--indent;
	printI("</resource>\n");

	return 0;
}

static int sh_nop(struct d_resource *ignored __attribute((unused)),
		  const char *unused __attribute((unused)))
{
	return 0;
}

static int sh_resources(struct d_resource *ignored __attribute((unused)),
			const char *unused __attribute((unused)))
{
	struct d_resource *res, *t;
	int first = 1;

	for_each_resource(res, t, config) {
		if (res->ignore)
			continue;
		if (is_drbd_top != res->stacked)
			continue;
		printf(first ? "%s" : " %s", esc(res->name));
		first = 0;
	}
	if (!first)
		printf("\n");

	return 0;
}

static int sh_resource(struct d_resource *res,
		       const char *unused __attribute((unused)))
{
	printf("%s\n", res->name);

	return 0;
}

static int sh_dev(struct d_resource *res,
		  const char *unused __attribute((unused)))
{
	printf("%s\n", res->me->device);

	return 0;
}

static int sh_udev(struct d_resource *res,
		   const char *unused __attribute((unused)))
{
	/* No shell escape necessary. Udev does not handle it anyways... */
	printf("RESOURCE=%s\n", res->name);

	if (!strncmp(res->me->device, "/dev/drbd", 9))
		printf("DEVICE=%s\n", res->me->device + 5);
	else
		printf("DEVICE=drbd%u\n", res->me->device_minor);

	if (!strncmp(res->me->disk, "/dev/", 5))
		printf("DISK=%s\n", res->me->disk + 5);
	else
		printf("DISK=%s\n", res->me->disk);

	return 0;
}

static int sh_minor(struct d_resource *res,
		    const char *unused __attribute((unused)))
{
	printf("%d\n", res->me->device_minor);

	return 0;
}

static int sh_ip(struct d_resource *res,
		 const char *unused __attribute((unused)))
{
	printf("%s\n", res->me->address);

	return 0;
}

static int sh_lres(struct d_resource *res,
		   const char *unused __attribute((unused)))
{
	if (!is_drbd_top) {
		fprintf(stderr,
			"sh-lower-resource only available in stacked mode\n");
		exit(E_usage);
	}
	if (!res->stacked) {
		fprintf(stderr, "'%s' is not stacked on this host (%s)\n",
			res->name, nodeinfo.nodename);
		exit(E_usage);
	}
	printf("%s\n", res->me->lower->name);

	return 0;
}

static int sh_ll_dev(struct d_resource *res,
		     const char *unused __attribute((unused)))
{
	printf("%s\n", res->me->disk);

	return 0;
}

static int sh_md_dev(struct d_resource *res,
		     const char *unused __attribute((unused)))
{
	char *r;

	if (strcmp("internal", res->me->meta_disk) == 0)
		r = res->me->disk;
	else
		r = res->me->meta_disk;

	printf("%s\n", r);

	return 0;
}

static int sh_md_idx(struct d_resource *res,
		     const char *unused __attribute((unused)))
{
	printf("%s\n", res->me->meta_index);

	return 0;
}

static int sh_b_pri(struct d_resource *res,
		    const char *unused __attribute((unused)))
{
	int i, rv;

	if (name_in_names(nodeinfo.nodename, res->become_primary_on) ||
	    name_in_names("both", res->become_primary_on)) {
		/* Opon connect resync starts, and both sides become primary at the same time.
		   One's try might be declined since an other state transition happens. Retry. */
		for (i = 0; i < 5; i++) {
			rv = adm_generic_s(res, "primary");
			if (rv == 0)
				return rv;
			sleep(1);
		}
		return rv;
	}
	return 0;
}

static int sh_mod_parms(struct d_resource *res __attribute((unused)),
			const char *unused __attribute((unused)))
{
	int mc = global_options.minor_count;

	if (mc == 0)
		mc = highest_minor + 11;
	if (mc < 32)
		mc = 32;
	printf("minor_count=%d\n", mc);
	return 0;
}

static void free_host_info(struct d_host_info *hi)
{
	if (!hi)
		return;

	free_names(hi->on_hosts);
	free(hi->device);
	free(hi->disk);
	free(hi->address);
	free(hi->address_family);
	free(hi->port);
	free(hi->meta_disk);
	free(hi->meta_index);
}

static void free_options(struct d_option *opts)
{
	struct d_option *f;
	while (opts) {
		free(opts->name);
		free(opts->value);
		f = opts;
		opts = opts->next;
		free(f);
	}
}

static void free_config(struct d_resource *res)
{
	struct d_resource *f, *t;
	struct d_host_info *host;

	for_each_resource(f, t, res) {
		free(f->name);
		free(f->protocol);
		free(f->device);
		free(f->disk);
		free(f->meta_disk);
		free(f->meta_index);
		for (host = f->all_hosts; host; host = host->next)
			free_host_info(host);
		free_options(f->net_options);
		free_options(f->disk_options);
		free_options(f->sync_options);
		free_options(f->startup_options);
		free_options(f->proxy_options);
		free_options(f->handlers);
		free(f);
	}
	if (common) {
		free_options(common->net_options);
		free_options(common->disk_options);
		free_options(common->sync_options);
		free_options(common->startup_options);
		free_options(common->proxy_options);
		free_options(common->handlers);
		free(common);
	}
	if (ifreq_list)
		free(ifreq_list);
}

static void expand_opts(struct d_option *co, struct d_option **opts)
{
	struct d_option *no;

	while (co) {
		if (!find_opt(*opts, co->name)) {
			// prepend new item to opts
			no = new_opt(strdup(co->name),
				     co->value ? strdup(co->value) : NULL);
			no->next = *opts;
			*opts = no;
		}
		co = co->next;
	}
}

static void expand_common(void)
{
	struct d_resource *res, *tmp;
	struct d_host_info *h;

	for_each_resource(res, tmp, config) {
		for (h = res->all_hosts; h; h = h->next) {
			if (!h->device)
				m_asprintf(&h->device, "/dev/drbd%u",
					   h->device_minor);
		}
	}

	if (!common)
		return;

	for_each_resource(res, tmp, config) {
		expand_opts(common->net_options, &res->net_options);
		expand_opts(common->disk_options, &res->disk_options);
		expand_opts(common->sync_options, &res->sync_options);
		expand_opts(common->startup_options, &res->startup_options);
		expand_opts(common->proxy_options, &res->proxy_options);
		expand_opts(common->handlers, &res->handlers);

		if (common->protocol && !res->protocol)
			res->protocol = strdup(common->protocol);

		if (common->stacked_timeouts)
			res->stacked_timeouts = 1;

		if (!res->become_primary_on)
			res->become_primary_on = common->become_primary_on;

	}
}

static void find_drbdcmd(char **cmd, char **pathes)
{
	char **path;

	path = pathes;
	while (*path) {
		if (access(*path, X_OK) == 0) {
			*cmd = *path;
			return;
		}
		path++;
	}

	fprintf(stderr, "Can not find command (drbdsetup/drbdmeta)\n");
	exit(E_exec_error);
}

static void alarm_handler(int __attribute((unused)) signo)
{
	alarm_raised = 1;
}

pid_t m_system(char **argv, int flags, struct d_resource *res)
{
	pid_t pid;
	int status, rv = -1;
	int timeout = 0;
	char **cmdline = argv;
	int pipe_fds[2];

	struct sigaction so;
	struct sigaction sa;

	sa.sa_handler = &alarm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (dry_run || verbose) {
		if (sh_varname && *cmdline)
			printf("%s=%s\n", sh_varname, shell_escape(res->name));
		while (*cmdline) {
			printf("%s ", shell_escape(*cmdline++));
		}
		printf("\n");
		if (dry_run)
			return 0;
	}

	/* flush stdout and stderr, so output of drbdadm
	 * and helper binaries is reported in order! */
	fflush(stdout);
	fflush(stderr);

	if (flags & (RETURN_STDOUT_FD | RETURN_STDERR_FD)) {
		if (pipe(pipe_fds) < 0) {
			perror("pipe");
			fprintf(stderr, "Error in pipe, giving up.\n");
			exit(E_exec_error);
		}
	}

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Can not fork\n");
		exit(E_exec_error);
	}
	if (pid == 0) {
		if (flags & RETURN_STDOUT_FD) {
			close(pipe_fds[0]);
			dup2(pipe_fds[1], 1);
		}
		if (flags & RETURN_STDERR_FD) {
			close(pipe_fds[0]);
			dup2(pipe_fds[1], 2);
		}
		if (flags & SUPRESS_STDERR)
			fclose(stderr);
		execvp(argv[0], argv);
		fprintf(stderr, "Can not exec\n");
		exit(E_exec_error);
	}

	if (flags & (RETURN_STDOUT_FD | RETURN_STDERR_FD))
		close(pipe_fds[1]);

	if (flags & SLEEPS_FINITE) {
		sigaction(SIGALRM, &sa, &so);
		alarm_raised = 0;
		switch (flags & SLEEPS_MASK) {
		case SLEEPS_SHORT:
			timeout = 5;
			break;
		case SLEEPS_LONG:
			timeout = COMM_TIMEOUT + 1;
			break;
		case SLEEPS_VERY_LONG:
			timeout = 600;
			break;
		default:
			fprintf(stderr, "logic bug in %s:%d\n", __FILE__,
				__LINE__);
			exit(E_thinko);
		}
		alarm(timeout);
	}

	if (flags == RETURN_PID) {
		return pid;
	}

	if (flags & (RETURN_STDOUT_FD | RETURN_STDERR_FD))
		return pipe_fds[0];

	while (1) {
		if (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR)
				break;
			if (alarm_raised) {
				alarm(0);
				sigaction(SIGALRM, &so, NULL);
				rv = 0x100;
				break;
			} else {
				fprintf(stderr, "logic bug in %s:%d\n",
					__FILE__, __LINE__);
				exit(E_exec_error);
			}
		} else {
			if (WIFEXITED(status)) {
				rv = WEXITSTATUS(status);
				break;
			}
		}
	}

	if (flags & SLEEPS_FINITE) {
		if (rv >= 10
		    && !(flags & (DONT_REPORT_FAILED | SUPRESS_STDERR))) {
			fprintf(stderr, "Command '");
			for (cmdline = argv; *cmdline; cmdline++) {
				fprintf(stderr, "%s", *cmdline);
				if (cmdline[1])
					fputc(' ', stderr);
			}
			if (alarm_raised) {
				fprintf(stderr,
					"' did not terminate within %u seconds\n",
					timeout);
				exit(E_exec_error);
			} else {
				fprintf(stderr,
					"' terminated with exit code %d\n", rv);
			}
		}
	}
	fflush(stdout);
	fflush(stderr);

	return rv;
}

#define NA(ARGC) \
  ({ if((ARGC) >= MAX_ARGS) { fprintf(stderr,"MAX_ARGS too small\n"); \
       exit(E_thinko); \
     } \
     (ARGC)++; \
  })

#define make_options(OPT) \
  while(OPT) { \
    if(OPT->value) { \
      ssprintf(argv[NA(argc)],"--%s=%s",OPT->name,OPT->value); \
    } else { \
      ssprintf(argv[NA(argc)],"--%s",OPT->name); \
    } \
    OPT=OPT->next; \
  }

#define make_address(ADDR, PORT, AF)		\
  if (strcmp(AF, "ipv4")) { \
    ssprintf(argv[NA(argc)],"%s:%s:%s", AF, ADDR, PORT); \
  } else { \
    ssprintf(argv[NA(argc)],"%s:%s", ADDR, PORT); \
  }

int adm_attach(struct d_resource *res, const char *unused __attribute((unused)))
{
	char *argv[MAX_ARGS];
	struct d_option *opt;
	int argc = 0;

	argv[NA(argc)] = drbdsetup;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = "disk";
	argv[NA(argc)] = res->me->disk;
	if (!strcmp(res->me->meta_disk, "internal")) {
		argv[NA(argc)] = res->me->disk;
	} else {
		argv[NA(argc)] = res->me->meta_disk;
	}
	argv[NA(argc)] = res->me->meta_index;
	argv[NA(argc)] = "--set-defaults";
	argv[NA(argc)] = "--create-device";
	opt = res->disk_options;
	make_options(opt);
	argv[NA(argc)] = 0;

	return m_system(argv, SLEEPS_LONG, res);
}

struct d_option *find_opt(struct d_option *base, char *name)
{
	while (base) {
		if (!strcmp(base->name, name)) {
			return base;
		}
		base = base->next;
	}
	return 0;
}

int adm_resize(struct d_resource *res, const char *unused __attribute((unused)))
{
	char *argv[MAX_ARGS];
	struct d_option *opt;
	int i, argc = 0;

	argv[NA(argc)] = drbdsetup;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = "resize";
	opt = find_opt(res->disk_options, "size");
	if (opt)
		ssprintf(argv[NA(argc)], "--%s=%s", opt->name, opt->value);
	for (i = 0; i < soi; i++) {
		argv[NA(argc)] = setup_opts[i];
	}
	argv[NA(argc)] = 0;

	return m_system(argv, SLEEPS_SHORT, res);
}

int _admm_generic(struct d_resource *res, const char *cmd, int flags)
{
	char *argv[MAX_ARGS];
	int argc = 0, i;

	argv[NA(argc)] = drbdmeta;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = "v08";
	if (!strcmp(res->me->meta_disk, "internal")) {
		argv[NA(argc)] = res->me->disk;
	} else {
		argv[NA(argc)] = res->me->meta_disk;
	}
	if (!strcmp(res->me->meta_index, "flexible")) {
		if (!strcmp(res->me->meta_disk, "internal")) {
			argv[NA(argc)] = "flex-internal";
		} else {
			argv[NA(argc)] = "flex-external";
		}
	} else {
		argv[NA(argc)] = res->me->meta_index;
	}
	argv[NA(argc)] = (char *)cmd;
	for (i = 0; i < soi; i++) {
		argv[NA(argc)] = setup_opts[i];
	}

	argv[NA(argc)] = 0;

	return m_system(argv, flags, res);
}

static int admm_generic(struct d_resource *res, const char *cmd)
{
	return _admm_generic(res, cmd, SLEEPS_VERY_LONG);
}

static int adm_generic(struct d_resource *res, const char *cmd, int flags)
{
	char *argv[MAX_ARGS];
	int argc = 0, i;

	argv[NA(argc)] = drbdsetup;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = (char *)cmd;
	for (i = 0; i < soi; i++) {
		argv[NA(argc)] = setup_opts[i];
	}
	argv[NA(argc)] = 0;

	setenv("DRBD_RESOURCE", res->name, 1);
	return m_system(argv, flags, res);
}

int adm_generic_s(struct d_resource *res, const char *cmd)
{
	return adm_generic(res, cmd, SLEEPS_SHORT);
}

int adm_status_xml(struct d_resource *res, const char *cmd)
{
	struct d_resource *r, *t;
	int rv = 0;

	if (!dry_run) {
		printf("<drbd-status version=\"%s\" api=\"%u\">\n",
		       REL_VERSION, API_VERSION);
		printf("<resources config_file=\"%s\">\n", config_save);
	}

	for_each_resource(r, t, res) {
		if (r->ignore)
			continue;
		rv = adm_generic(r, cmd, SLEEPS_SHORT);
		if (rv)
			break;
	}

	if (!dry_run)
		printf("</resources>\n</drbd-status>\n");
	return rv;
}

int sh_status(struct d_resource *res, const char *cmd)
{
	struct d_resource *r, *t;
	int rv = 0;

	if (!dry_run) {
		printf("_drbd_version=%s\n_drbd_api=%u\n",
		       shell_escape(REL_VERSION), API_VERSION);
		printf("_config_file=%s\n\n", shell_escape(config_save));
	}

	for_each_resource(r, t, res) {
		if (r->ignore)
			continue;
		printf("_stacked_on=%s\n", r->stacked && r->me->lower ?
		       shell_escape(r->me->lower->name) : "");
		printf("_stacked_on_device=%s\n", r->stacked && r->me->lower ?
		       shell_escape(r->me->lower->me->device) : "");
		if (r->stacked && r->me->lower)
			printf("_stacked_on_minor=%d\n",
			       r->me->lower->me->device_minor);
		else
			printf("_stacked_on_minor=\n");
		rv = adm_generic(r, cmd, SLEEPS_SHORT);
		if (rv)
			break;
	}

	return rv;
}

int adm_generic_l(struct d_resource *res, const char *cmd)
{
	return adm_generic(res, cmd, SLEEPS_LONG);
}

static int adm_outdate(struct d_resource *res, const char *cmd)
{
	int rv;

	rv = adm_generic(res, cmd, SLEEPS_SHORT | SUPRESS_STDERR);
	/* special cases for outdate:
	 * 17: drbdsetup outdate, but is primary and thus cannot be outdated.
	 *  5: drbdsetup outdate, and is inconsistent or worse anyways. */
	if (rv == 17)
		return rv;

	if (rv == 5) {
		/* That might mean it is diskless. */
		rv = admm_generic(res, cmd);
		if (rv)
			rv = 5;
		return rv;
	}

	if (rv || dry_run) {
		rv = admm_generic(res, cmd);
	}
	return rv;
}

static int adm_generic_b(struct d_resource *res, const char *cmd)
{
	char buffer[4096];
	int fd, status, rv = 0, rr, s = 0;

	fd = adm_generic(res, cmd, SLEEPS_SHORT | RETURN_STDERR_FD);

	if (fd < 0) {
		fprintf(stderr, "Strange: got negative fd.\n");
		exit(E_thinko);
	}

	if (!dry_run) {
		while (1) {
			rr = read(fd, buffer + s, 4096 - s);
			if (rr <= 0)
				break;
			s += rr;
		}

		close(fd);
		waitpid(0, &status, WNOHANG);

		if (WIFEXITED(status))
			rv = WEXITSTATUS(status);
		if (alarm_raised) {
			rv = 0x100;
		}
	}

	if (rv == 11) {
		/* Some state transition error, report it ... */
		rr = write(fileno(stderr), buffer, s);
		return rv;
	}

	if (rv || dry_run) {
		/* On other errors
		   rv = 10 .. no minor allocated
		   rv = 20 .. module not loaded
		   rv = 16 .. we are diskless here
		   retry with drbdmeta.
		 */
		rv = admm_generic(res, cmd);
	}
	return rv;
}

static int adm_khelper(struct d_resource *res, const char *cmd)
{
	int rv = 0;
	char *sh_cmd;
	char minor_string[8];
	char *argv[] = { "/bin/sh", "-c", NULL, NULL };

	if (!res->peer) {
		/* Since 8.3.2 we get DRBD_PEER_AF and DRBD_PEER_ADDRESS from the kernel.
		   If we do not know the peer by now, use these to find the peer. */
		struct d_host_info *host;
		char *peer_address = getenv("DRBD_PEER_ADDRESS");
		char *peer_af = getenv("DRBD_PEER_AF");

		if (peer_address && peer_af) {
			for (host = res->all_hosts; host; host = host->next) {
				if (!strcmp(host->address_family, peer_af) &&
				    !strcmp(host->address, peer_address)) {
					res->peer = host;
					break;
				}
			}
		}
	}

	if (res->peer) {
		setenv("DRBD_PEER_AF", res->peer->address_family, 1);	/* since 8.3.0 */
		setenv("DRBD_PEER_ADDRESS", res->peer->address, 1);	/* since 8.3.0 */
		setenv("DRBD_PEER", res->peer->on_hosts->name, 1);	/* deprecated */
		setenv("DRBD_PEERS", names_to_str(res->peer->on_hosts), 1);
			/* since 8.3.0, but not usable when using a config with "floating" statements. */
	}

	snprintf(minor_string, sizeof(minor_string), "%u", res->me->device_minor);
	setenv("DRBD_RESOURCE", res->name, 1);
	setenv("DRBD_MINOR", minor_string, 1);
	setenv("DRBD_CONF", config_save, 1);

	if ((sh_cmd = get_opt_val(res->handlers, cmd, NULL))) {
		argv[2] = sh_cmd;
		rv = m_system(argv, SLEEPS_VERY_LONG, res);
	}
	return rv;
}

// need to convert discard-node-nodename to discard-local or discard-remote.
void convert_discard_opt(struct d_resource *res)
{
	struct d_option *opt;

	if (res == NULL)
		return;

	if ((opt = find_opt(res->net_options, "after-sb-0pri"))) {
		if (!strncmp(opt->value, "discard-node-", 13)) {
			if (!strcmp(nodeinfo.nodename, opt->value + 13)) {
				free(opt->value);
				opt->value = strdup("discard-local");
			} else {
				free(opt->value);
				opt->value = strdup("discard-remote");
			}
		}
	}
}

int adm_connect(struct d_resource *res,
		const char *unused __attribute((unused)))
{
	char *argv[MAX_ARGS];
	struct d_option *opt;
	int i;
	int argc = 0;

	argv[NA(argc)] = drbdsetup;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = "net";
	make_address(res->me->address, res->me->port, res->me->address_family);
	if (res->me->proxy) {
		make_address(res->me->proxy->inside_addr,
			     res->me->proxy->inside_port,
			     res->me->proxy->inside_af);
	} else if (res->peer) {
		make_address(res->peer->address, res->peer->port,
			     res->peer->address_family);
	} else if (dry_run > 1) {
		argv[NA(argc)] = "N/A";
	} else {
		fprintf(stderr, "resource %s: cannot change network config without knowing my peer.\n", res->name);
		return dry_run ? 0 : 20;
	}
	argv[NA(argc)] = res->protocol;

	argv[NA(argc)] = "--set-defaults";
	argv[NA(argc)] = "--create-device";
	opt = res->net_options;
	make_options(opt);

	for (i = 0; i < soi; i++) {
		argv[NA(argc)] = setup_opts[i];
	}

	argv[NA(argc)] = 0;

	return m_system(argv, SLEEPS_SHORT, res);
}

struct d_resource *res_by_name(const char *name);

struct d_option *del_opt(struct d_option *base, struct d_option *item)
{
	struct d_option *i;
	if (base == item) {
		base = item->next;
		free(item->name);
		free(item->value);
		free(item);
		return base;
	}

	for (i = base; i; i = i->next) {
		if (i->next == item) {
			i->next = item->next;
			free(item->name);
			free(item->value);
			free(item);
			return base;
		}
	}
	return base;
}

// Need to convert after from resourcename to minor_number.
void convert_after_option(struct d_resource *res)
{
	struct d_option *opt;
	struct d_resource *depends_on_res;

	if (res == NULL)
		return;

	if ((opt = find_opt(res->sync_options, "after"))) {
		char *ptr;
		depends_on_res = res_by_name(opt->value);
		if (depends_on_res->ignore) {
			res->sync_options = del_opt(res->sync_options, opt);
		} else {
			ssprintf(ptr, "%d", depends_on_res->me->device_minor);
			free(opt->value);
			opt->value = strdup(ptr);
		}
	}
}

static int do_proxy(struct d_resource *res, int do_up)
{
	char *argv[MAX_ARGS];
	int argc = 0, rv;
	struct d_option *opt;

	if (!res->me->proxy) {
		if (all_resources)
			return 0;
		fprintf(stderr,
			"There is no proxy config for host %s in resource %s.\n",
			nodeinfo.nodename, res->name);
		exit(E_config_invalid);
	}

	if (!name_in_names(nodeinfo.nodename, res->me->proxy->on_hosts)) {
		if (all_resources)
			return 0;
		fprintf(stderr,
			"The proxy config in resource %s is not for %s.\n",
			res->name, nodeinfo.nodename);
		exit(E_config_invalid);
	}

	if (!res->peer) {
		fprintf(stderr, "Cannot determine the peer in resource %s.\n",
			res->name);
		exit(E_config_invalid);
	}

	argv[NA(argc)] = drbd_proxy_ctl;
	argv[NA(argc)] = "-c";
	if (do_up) {
		ssprintf(argv[NA(argc)],
			 "add connection %s-%s-%s %s:%s %s:%s %s:%s %s:%s",
			 names_to_str_c(res->me->proxy->on_hosts, '_'),
			 res->name,
			 names_to_str_c(res->peer->proxy->on_hosts, '_'),
			 res->me->proxy->inside_addr,
			 res->me->proxy->inside_port,
			 res->peer->proxy->outside_addr,
			 res->peer->proxy->outside_port,
			 res->me->proxy->outside_addr,
			 res->me->proxy->outside_port, res->me->address,
			 res->me->port);
	} else {
		ssprintf(argv[NA(argc)],
			 "del connection %s-%s-%s",
			 names_to_str_c(res->me->proxy->on_hosts, '_'),
			 res->name,
			 names_to_str_c(res->peer->proxy->on_hosts, '_'));
	}
	argv[NA(argc)] = 0;

	rv = m_system(argv, SLEEPS_SHORT, res);
	if (rv != 0)
		return rv;

	if (!do_up)
		return rv;

	argc = 0;
	argv[NA(argc)] = drbd_proxy_ctl;
	opt = res->proxy_options;
	while (opt) {
		argv[NA(argc)] = "-c";
		ssprintf(argv[NA(argc)], "set %s %s-%s-%s %s",
			 opt->name, names_to_str_c(res->me->proxy->on_hosts,
						   '_'), res->name,
			 names_to_str_c(res->peer->proxy->on_hosts, '_'),
			 opt->value);
		opt = opt->next;
	}
	argv[NA(argc)] = 0;
	if (argc > 2)
		return m_system(argv, SLEEPS_SHORT, res);
	return rv;
}

static int adm_proxy_up(struct d_resource *res,
			const char *unused __attribute((unused)))
{
	return do_proxy(res, 1);
}

static int adm_proxy_down(struct d_resource *res,
			  const char *unused __attribute((unused)))
{
	return do_proxy(res, 0);
}

int adm_syncer(struct d_resource *res, const char *unused __attribute((unused)))
{
	char *argv[MAX_ARGS];
	struct d_option *opt;
	int i, argc = 0;

	argv[NA(argc)] = drbdsetup;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = "syncer";

	argv[NA(argc)] = "--set-defaults";
	argv[NA(argc)] = "--create-device";
	opt = res->sync_options;
	make_options(opt);

	for (i = 0; i < soi; i++) {
		argv[NA(argc)] = setup_opts[i];
	}

	argv[NA(argc)] = 0;

	return m_system(argv, SLEEPS_SHORT, res);
}

static int adm_up(struct d_resource *res,
		  const char *unused __attribute((unused)))
{
	schedule_dcmd(adm_attach, res, "attach", 0);
	schedule_dcmd(adm_syncer, res, "syncer", 1);
	schedule_dcmd(adm_connect, res, "connect", 2);

	return 0;
}

/* The stacked-timeouts switch in the startup sections allows us
   to enforce the use of the specified timeouts instead the use
   of a sane value. Should only be used if the third node should
   never become primary. */
static int adm_wait_c(struct d_resource *res,
		      const char *unused __attribute((unused)))
{
	char *argv[MAX_ARGS];
	struct d_option *opt;
	int argc = 0, rv;

	argv[NA(argc)] = drbdsetup;
	ssprintf(argv[NA(argc)], "%d", res->me->device_minor);
	argv[NA(argc)] = "wait-connect";
	if (is_drbd_top && !res->stacked_timeouts) {
		unsigned long timeout = 20;
		if ((opt = find_opt(res->net_options, "connect-int"))) {
			timeout = strtoul(opt->value, NULL, 10);
			// one connect-interval? two?
			timeout *= 2;
		}
		argv[argc++] = "-t";
		ssprintf(argv[argc], "%lu", timeout);
		argc++;
	} else {
		opt = res->startup_options;
		make_options(opt);
	}
	argv[NA(argc)] = 0;

	rv = m_system(argv, SLEEPS_FOREVER, res);

	return rv;
}

static unsigned minor_by_id(const char *id)
{
	if (strncmp(id, "minor-", 6))
		return -1U;
	return m_strtoll(id + 6, 1);
}

struct d_resource *res_by_minor(const char *id)
{
	struct d_resource *res, *t;
	unsigned int mm;

	mm = minor_by_id(id);
	if (mm == -1U)
		return NULL;

	for_each_resource(res, t, config) {
		if (res->ignore)
			continue;
		if (mm == res->me->device_minor) {
			is_drbd_top = res->stacked;
			return res;
		}
	}
	return NULL;
}

struct d_resource *res_by_name(const char *name)
{
	struct d_resource *res, *t;

	for_each_resource(res, t, config) {
		if (strcmp(name, res->name) == 0)
			return res;
	}
	return NULL;
}

/* In case a child exited, or exits, its return code is stored as
   negative number in the pids[i] array */
static int childs_running(pid_t * pids, int opts)
{
	int i = 0, wr, rv = 0, status;

	for (i = 0; i < nr_resources; i++) {
		if (pids[i] <= 0)
			continue;
		wr = waitpid(pids[i], &status, opts);
		if (wr == -1) {	// Wait error.
			if (errno == ECHILD) {
				printf("No exit code for %d\n", pids[i]);
				pids[i] = 0;	// Child exited before ?
				continue;
			}
			perror("waitpid");
			exit(E_exec_error);
		}
		if (wr == 0)
			rv = 1;	// Child still running.
		if (wr > 0) {
			pids[i] = 0;
			if (WIFEXITED(status))
				pids[i] = -WEXITSTATUS(status);
			if (WIFSIGNALED(status))
				pids[i] = -1000;
		}
	}
	return rv;
}

static void kill_childs(pid_t * pids)
{
	int i;

	for (i = 0; i < nr_resources; i++) {
		if (pids[i] <= 0)
			continue;
		kill(pids[i], SIGINT);
	}
}

/*
  returns:
  -1 ... all childs terminated
   0 ... timeout expired
   1 ... a string was read
 */
int gets_timeout(pid_t * pids, char *s, int size, int timeout)
{
	int pr, rr, n = 0;
	struct pollfd pfd;

	if (s) {
		pfd.fd = fileno(stdin);
		pfd.events = POLLIN | POLLHUP | POLLERR | POLLNVAL;
		n = 1;
	}

	if (!childs_running(pids, WNOHANG)) {
		pr = -1;
		goto out;
	}

	do {
		pr = poll(&pfd, n, timeout);

		if (pr == -1) {	// Poll error.
			if (errno == EINTR) {
				if (childs_running(pids, WNOHANG))
					continue;
				goto out;	// pr = -1 here.
			}
			perror("poll");
			exit(E_exec_error);
		}
	} while (pr == -1);

	if (pr == 1) {		// Input available.
		rr = read(fileno(stdin), s, size - 1);
		if (rr == -1) {
			perror("read");
			exit(E_exec_error);
		}
		s[rr] = 0;
	}

out:
	return pr;
}

static char *get_opt_val(struct d_option *base, const char *name, char *def)
{
	while (base) {
		if (!strcmp(base->name, name)) {
			return base->value;
		}
		base = base->next;
	}
	return def;
}

void chld_sig_hand(int __attribute((unused)) unused)
{
	// do nothing. But interrupt systemcalls :)
}

static int check_exit_codes(pid_t * pids)
{
	struct d_resource *res, *t;
	int i = 0, rv = 0;

	for_each_resource(res, t, config) {
		if (res->ignore)
			continue;
		if (is_drbd_top != res->stacked)
			continue;
		if (pids[i] == -5 || pids[i] == -1000) {
			pids[i] = 0;
		}
		if (pids[i] == -20)
			rv = 20;
		i++;
	}
	return rv;
}

static int adm_wait_ci(struct d_resource *ignored __attribute((unused)),
		       const char *unused __attribute((unused)))
{
	struct d_resource *res, *t;
	char *argv[20], answer[40];
	pid_t *pids;
	struct d_option *opt;
	int rr, wtime, argc, i = 0;
	time_t start;
	int saved_stdin, saved_stdout, fd;

	struct sigaction so, sa;

	saved_stdin = -1;
	saved_stdout = -1;
	if (no_tty) {
		fprintf(stderr,
			"WARN: stdin/stdout is not a TTY; using /dev/console");
		fprintf(stdout,
			"WARN: stdin/stdout is not a TTY; using /dev/console");
		saved_stdin = dup(fileno(stdin));
		if (saved_stdin == -1)
			perror("dup(stdin)");
		saved_stdout = dup(fileno(stdout));
		if (saved_stdin == -1)
			perror("dup(stdout)");
		fd = open("/dev/console", O_RDONLY);
		if (fd == -1)
			perror("open('/dev/console, O_RDONLY)");
		dup2(fd, fileno(stdin));
		fd = open("/dev/console", O_WRONLY);
		if (fd == -1)
			perror("open('/dev/console, O_WRONLY)");
		dup2(fd, fileno(stdout));
	}

	sa.sa_handler = chld_sig_hand;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, &so);

	pids = alloca(nr_resources * sizeof(pid_t));
	/* alloca can not fail, it can "only" overflow the stack :)
	 * but it needs to be initialized anyways! */
	memset(pids, 0, nr_resources * sizeof(pid_t));

	for_each_resource(res, t, config) {
		if (res->ignore)
			continue;
		if (is_drbd_top != res->stacked)
			continue;
		argc = 0;
		argv[NA(argc)] = drbdsetup;
		ssprintf(argv[NA(argc)], "%d", res->me->device_minor);

		argv[NA(argc)] = "wait-connect";
		opt = res->startup_options;
		make_options(opt);
		argv[NA(argc)] = 0;

		pids[i++] = m_system(argv, RETURN_PID, res);
	}

	wtime = global_options.dialog_refresh ? : -1;

	start = time(0);
	for (i = 0; i < 10; i++) {
		// no string, but timeout
		rr = gets_timeout(pids, 0, 0, 1 * 1000);
		if (rr < 0)
			break;
		putchar('.');
		fflush(stdout);
		check_exit_codes(pids);
	}

	if (rr == 0) {
		printf
		    ("\n***************************************************************\n"
		     " DRBD's startup script waits for the peer node(s) to appear.\n"
		     " - In case this node was already a degraded cluster before the\n"
		     "   reboot the timeout is %s seconds. [degr-wfc-timeout]\n"
		     " - If the peer was available before the reboot the timeout will\n"
		     "   expire after %s seconds. [wfc-timeout]\n"
		     "   (These values are for resource '%s'; 0 sec -> wait forever)\n",
		     get_opt_val(config->startup_options, "degr-wfc-timeout",
				 "0"), get_opt_val(config->startup_options,
						   "wfc-timeout", "0"),
		     config->name);

		printf(" To abort waiting enter 'yes' [ -- ]:");
		do {
			printf("\e[s\e[31G[%4d]:\e[u", (int)(time(0) - start));	// Redraw sec.
			fflush(stdout);
			rr = gets_timeout(pids, answer, 40, wtime * 1000);
			check_exit_codes(pids);

			if (rr == 1) {
				if (!strcmp(answer, "yes\n")) {
					kill_childs(pids);
					childs_running(pids, 0);
					check_exit_codes(pids);
					rr = -1;
				} else {
					printf
					    (" To abort waiting enter 'yes' [ -- ]:");
				}
			}
		} while (rr != -1);
		printf("\n");
	}

	if (saved_stdin != -1) {
		dup2(saved_stdin, fileno(stdin));
		dup2(saved_stdout, fileno(stdout));
	}

	return 0;
}

static void print_cmds(int level)
{
	size_t i;
	int j = 0;

	for (i = 0; i < ARRY_SIZE(cmds); i++) {
		if (cmds[i].show_in_usage != level)
			continue;
		if (j++ % 2) {
			printf("%-35s\n", cmds[i].name);
		} else {
			printf(" %-35s", cmds[i].name);
		}
	}
	if (j % 2)
		printf("\n");
}

static int hidden_cmds(struct d_resource *ignored __attribute((unused)),
		       const char *ignored2 __attribute((unused)))
{
	printf("\nThese additional commands might be useful for writing\n"
	       "nifty shell scripts around drbdadm:\n\n");

	print_cmds(2);

	printf("\nThese commands are used by the kernel part of DRBD to\n"
	       "invoke user mode helper programs:\n\n");

	print_cmds(3);

	printf
	    ("\nThese commands ought to be used by experts and developers:\n\n");

	print_cmds(4);

	printf("\n");

	exit(0);
}

void print_usage_and_exit(const char *addinfo)
{
	struct option *opt;

	printf("\nUSAGE: %s [OPTION...] [-- DRBDSETUP-OPTION...] COMMAND "
	       "{all|RESOURCE...}\n\n" "OPTIONS:\n", progname);

	opt = admopt;
	while (opt->name) {
		if (opt->has_arg == required_argument)
			printf(" {--%s|-%c} val\n", opt->name, opt->val);
		else
			printf(" {--%s|-%c}\n", opt->name, opt->val);
		opt++;
	}

	printf("\nCOMMANDS:\n");

	print_cmds(1);

	printf("\nVersion: " REL_VERSION " (api:%d)\n%s\n",
	       API_VERSION, drbd_buildtag());

	if (addinfo)
		printf("\n%s\n", addinfo);

	exit(E_usage);
}

/*
 * I'd really rather parse the output of
 *   ip -o a s
 * once, and be done.
 * But anyways....
 */

static struct ifreq *get_ifreq(void)
{
	int sockfd, num_ifaces;
	struct ifreq *ifr;
	struct ifconf ifc;
	size_t buf_size;

	if (0 > (sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))) {
		perror("Cannot open socket");
		exit(EXIT_FAILURE);
	}

	num_ifaces = 0;
	ifc.ifc_req = NULL;

	/* realloc buffer size until no overflow occurs  */
	do {
		num_ifaces += 16;	/* initial guess and increment */
		buf_size = ++num_ifaces * sizeof(struct ifreq);
		ifc.ifc_len = buf_size;
		if (NULL == (ifc.ifc_req = realloc(ifc.ifc_req, ifc.ifc_len))) {
			fprintf(stderr, "Out of memory.\n");
			return NULL;
		}
		if (ioctl(sockfd, SIOCGIFCONF, &ifc)) {
			perror("ioctl SIOCFIFCONF");
			free(ifc.ifc_req);
			return NULL;
		}
	} while (buf_size <= (size_t) ifc.ifc_len);

	num_ifaces = ifc.ifc_len / sizeof(struct ifreq);
	/* Since we allocated at least one more than necessary,
	 * this serves as a stop marker for the iteration in
	 * have_ip() */
	ifc.ifc_req[num_ifaces].ifr_name[0] = 0;
	for (ifr = ifc.ifc_req; ifr->ifr_name[0] != 0; ifr++) {
		/* we only want to look up the presence or absence of a certain address
		 * here. but we want to skip "down" interfaces.  if an interface is down,
		 * we store an invalid sa_family, so the lookup will skip it.
		 */
		struct ifreq ifr_for_flags = *ifr;	/* get a copy to work with */
		if (ioctl(sockfd, SIOCGIFFLAGS, &ifr_for_flags) < 0) {
			perror("ioctl SIOCGIFFLAGS");
			ifr->ifr_addr.sa_family = -1;	/* what's wrong here? anyways: skip */
			continue;
		}
		if (!(ifr_for_flags.ifr_flags & IFF_UP)) {
			ifr->ifr_addr.sa_family = -1;	/* is not up: skip */
			continue;
		}
	}
	close(sockfd);
	return ifc.ifc_req;
}

int have_ip_ipv4(const char *ip)
{
	struct ifreq *ifr;
	struct in_addr query_addr;

	query_addr.s_addr = inet_addr(ip);

	if (!ifreq_list)
		ifreq_list = get_ifreq();

	for (ifr = ifreq_list; ifr && ifr->ifr_name[0] != 0; ifr++) {
		/* SIOCGIFCONF only supports AF_INET */
		struct sockaddr_in *list_addr =
		    (struct sockaddr_in *)&ifr->ifr_addr;
		if (ifr->ifr_addr.sa_family != AF_INET)
			continue;
		if (query_addr.s_addr == list_addr->sin_addr.s_addr)
			return 1;
	}
	return 0;
}

int have_ip_ipv6(const char *ip)
{
	FILE *if_inet6;
	struct in6_addr addr6, query_addr;
	unsigned int b[4];
	char name[20];
	int i;

	if (inet_pton(AF_INET6, ip, &query_addr) <= 0)
		return 0;

#define PROC_IF_INET6 "/proc/net/if_inet6"
	if_inet6 = fopen(PROC_IF_INET6, "r");
	if (!if_inet6) {
		if (errno != ENOENT)
			perror("open of " PROC_IF_INET6 " failed:");
#undef PROC_IF_INET6
		return 0;
	}

	while (fscanf
	       (if_inet6,
		X32(08) X32(08) X32(08) X32(08) " %*02x %*02x %*02x %*02x %s",
		b, b + 1, b + 2, b + 3, name) > 0) {
		for (i = 0; i < 4; i++)
			addr6.s6_addr32[i] = cpu_to_be32(b[i]);

		if (memcmp(&query_addr, &addr6, sizeof(struct in6_addr)) == 0) {
			fclose(if_inet6);
			return 1;
		}
	}
	fclose(if_inet6);
	return 0;
}

int have_ip(const char *af, const char *ip)
{
	if (!strcmp(af, "ipv4"))
		return have_ip_ipv4(ip);
	else if (!strcmp(af, "ipv6"))
		return have_ip_ipv6(ip);

	return 1;		/* SCI */
}

void verify_ips(struct d_resource *res)
{
	if (global_options.disable_ip_verification)
		return;
	if (dry_run == 1 || do_verify_ips == 0)
		return;
	if (res->ignore)
		return;
	if (res->stacked && !is_drbd_top)
		return;

	if (!have_ip(res->me->address_family, res->me->address)) {
		ENTRY e, *ep;
		e.key = e.data = ep = NULL;
		m_asprintf(&e.key, "%s:%s", res->me->address, res->me->port);
		hsearch_r(e, FIND, &ep, &global_htable);
		fprintf(stderr, "%s: in resource %s, on %s:\n\t"
			"IP %s not found on this host.\n",
			ep ? (char *)ep->data : res->config_file,
			res->name, names_to_str(res->me->on_hosts),
			res->me->address);
		if (INVALID_IP_IS_INVALID_CONF)
			config_valid = 0;
	}
}

static char *conf_file[] = {
	"/etc/drbd-83.conf",
	"/etc/drbd-82.conf",
	"/etc/drbd-08.conf",
	"/etc/drbd.conf",
	0
};

int sanity_check_abs_cmd(char *cmd_name)
{
	struct stat sb;

	if (stat(cmd_name, &sb)) {
		/* If stat fails, just ignore this sanity check,
		 * we are still iterating over $PATH probably. */
		return 0;
	}

	if (!sb.st_mode & S_ISUID || sb.st_mode & S_IXOTH || sb.st_gid == 0) {
		static int did_header = 0;
		if (!did_header)
			fprintf(stderr,
				"WARN:\n"
				"  You are using the 'drbd-peer-outdater' as fence-peer program.\n"
				"  If you use that mechanism the dopd heartbeat plugin program needs\n"
				"  to be able to call drbdsetup and drbdmeta with root privileges.\n\n"
				"  You need to fix this with these commands:\n");
		did_header = 1;
		fprintf(stderr,
			"  chgrp haclient %s\n"
			"  chmod o-x %s\n"
			"  chmod u+s %s\n\n", cmd_name, cmd_name, cmd_name);
	}
	return 1;
}

void sanity_check_cmd(char *cmd_name)
{
	char *path, *pp, *c;
	char abs_path[100];

	if (strchr(cmd_name, '/')) {
		sanity_check_abs_cmd(cmd_name);
	} else {
		path = pp = c = strdup(getenv("PATH"));

		while (1) {
			c = strchr(pp, ':');
			if (c)
				*c = 0;
			snprintf(abs_path, 100, "%s/%s", pp, cmd_name);
			if (sanity_check_abs_cmd(abs_path))
				break;
			if (!c)
				break;
			c++;
			if (!*c)
				break;
			pp = c;
		}
		free(path);
	}
}

/* if the config file is not readable by haclient,
 * dopd cannot work.
 * NOTE: we assume that any gid != 0 will be the group dopd will run as,
 * typically haclient. */
void sanity_check_conf(char *c)
{
	struct stat sb;

	/* if we cannot stat the config file,
	 * we have other things to worry about. */
	if (stat(c, &sb))
		return;

	/* permissions are funny: if it is world readable,
	 * but not group readable, and it belongs to my group,
	 * I am denied access.
	 * For the file to be readable by dopd (hacluster:haclient),
	 * it is not enough to be world readable. */

	/* ok if world readable, and NOT group haclient (see NOTE above) */
	if (sb.st_mode & S_IROTH && sb.st_gid == 0)
		return;

	/* ok if group readable, and group haclient (see NOTE above) */
	if (sb.st_mode & S_IRGRP && sb.st_gid != 0)
		return;

	fprintf(stderr,
		"WARN:\n"
		"  You are using the 'drbd-peer-outdater' as fence-peer program.\n"
		"  If you use that mechanism the dopd heartbeat plugin program needs\n"
		"  to be able to read the drbd.config file.\n\n"
		"  You need to fix this with these commands:\n"
		"  chgrp haclient %s\n" "  chmod g+r %s\n\n", c, c);
}

void sanity_check_perm()
{
	static int checked = 0;
	if (checked)
		return;

	sanity_check_cmd(drbdsetup);
	sanity_check_cmd(drbdmeta);
	sanity_check_conf(config_file);
	checked = 1;
}

void validate_resource(struct d_resource *res)
{
	struct d_option *opt;
	struct d_name *bpo;

	if (!res->protocol) {
		if (!common || !common->protocol) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n\tprotocol definition missing.\n",
				res->config_file, res->start_line, res->name);
			config_valid = 0;
		}		/* else:
				 * may not have been expanded yet for "dump" subcommand */
	} else {
		res->protocol[0] = toupper(res->protocol[0]);
	}
	if ((opt = find_opt(res->sync_options, "after"))) {
		if (res_by_name(opt->value) == NULL) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n\tresource '%s' mentioned in "
				"'after' option is not known.\n",
				res->config_file, res->start_line, res->name,
				opt->value);
			config_valid = 0;
		}
	}
	if (res->ignore)
		return;
	if (!res->me) {
		fprintf(stderr,
			"%s:%d: in resource %s:\n\tmissing section 'on %s { ... }'.\n",
			res->config_file, res->start_line, res->name,
			nodeinfo.nodename);
		config_valid = 0;
	}
	// need to verify that in the discard-node-nodename options only known
	// nodenames are mentioned.
	if ((opt = find_opt(res->net_options, "after-sb-0pri"))) {
		if (!strncmp(opt->value, "discard-node-", 13)) {
			if (res->peer &&
			    !name_in_names(opt->value + 13, res->peer->on_hosts)
			    && !name_in_names(opt->value + 13,
					      res->me->on_hosts)) {
				fprintf(stderr,
					"%s:%d: in resource %s:\n\t"
					"the nodename in the '%s' option is "
					"not known.\n\t"
					"valid nodenames are: '%s %s'.\n",
					res->config_file, res->start_line,
					res->name, opt->value,
					names_to_str(res->me->on_hosts),
					names_to_str(res->peer->on_hosts));
				config_valid = 0;
			}
		}
	}

	if ((opt = find_opt(res->handlers, "fence-peer"))) {
		if (strstr(opt->value, "drbd-peer-outdater"))
			sanity_check_perm();
	}

	opt = find_opt(res->net_options, "allow-two-primaries");
	if (name_in_names("both", res->become_primary_on) && opt == NULL) {
		fprintf(stderr,
			"%s:%d: in resource %s:\n"
			"become-primary-on is set to both, but allow-two-primaries "
			"is not set.\n", res->config_file, res->start_line,
			res->name);
		config_valid = 0;
	}

	if (res->peer
	    && ((res->me->proxy == NULL) != (res->peer->proxy == NULL))) {
		fprintf(stderr,
			"%s:%d: in resource %s:\n\t"
			"Either both 'on' sections must contain a proxy subsection, or none.\n",
			res->config_file, res->start_line, res->name);
		config_valid = 0;
	}

	for (bpo = res->become_primary_on; bpo; bpo = bpo->next) {
		if (res->peer &&
		    !name_in_names(bpo->name, res->me->on_hosts) &&
		    !name_in_names(bpo->name, res->peer->on_hosts) &&
		    strcmp(bpo->name, "both")) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n\t"
				"become-primary-on contains '%s', which is not named with the 'on' sections.\n",
				res->config_file, res->start_line, res->name,
				bpo->name);
			config_valid = 0;
		}
	}
}

static void global_validate_maybe_expand_die_if_invalid(int expand)
{
	struct d_resource *res, *tmp;
	for_each_resource(res, tmp, config) {
		validate_resource(res);
		if (!config_valid)
			exit(E_config_invalid);
		if (expand) {
			convert_after_option(res);
			convert_discard_opt(res);
		}
	}
}

/*
 * returns a pointer to an malloced area that contains
 * an absolute, canonical, version of path.
 * aborts if any allocation or syscall fails.
 * return value should be free()d, once no longer needed.
 */
char *canonify_path(char *path)
{
	int cwd_fd = -1;
	char *last_slash;
	char *tmp;
	char *that_wd;
	char *abs_path;
	int len;

	if (!path || !path[0]) {
		fprintf(stderr, "cannot canonify an empty path\n");
		exit(E_usage);
	}

	tmp = strdupa(path);
	last_slash = strrchr(tmp, '/');

	if (last_slash) {
		*last_slash++ = '\0';
		cwd_fd = open(".", O_RDONLY);
		if (cwd_fd < 0) {
			fprintf(stderr, "open(\".\") failed: %m\n");
			exit(E_usage);
		}
		if (chdir(tmp)) {
			fprintf(stderr, "chdir(\"%s\") failed: %m\n", tmp);
			exit(E_usage);
		}
	} else {
		last_slash = tmp;
	}

	that_wd = getcwd(NULL, 0);
	if (!that_wd) {
		fprintf(stderr, "getcwd() failed: %m\n");
		exit(E_usage);
	}

	if (!strcmp("/", that_wd))
		len = asprintf(&abs_path, "/%s", last_slash);
	else
		len = asprintf(&abs_path, "%s/%s", that_wd, last_slash);

	if (len < 0) {
		fprintf(stderr, "out of memory during asprintf in %s\n",
			__func__);
		exit(E_usage);
	}

	free(that_wd);
	if (cwd_fd >= 0) {
		if (fchdir(cwd_fd) < 0) {
			fprintf(stderr, "fchdir() failed: %m\n");
			exit(E_usage);
		}
	}

	return abs_path;
}

void assign_command_names_from_argv0(char **argv)
{
	/* in case drbdadm is called with an absolute or relative pathname
	 * look for the drbdsetup binary in the same location,
	 * otherwise, just let execvp sort it out... */
	if ((progname = strrchr(argv[0], '/')) == 0) {
		progname = argv[0];
		drbdsetup = strdup("drbdsetup");
		drbdmeta = strdup("drbdmeta");
		drbd_proxy_ctl = strdup("drbd-proxy-ctl");
	} else {
		struct cmd_helper {
			char *name;
			char **var;
		};
		struct cmd_helper helpers[] = {
			{"drbdsetup", &drbdsetup},
			{"drbdmeta", &drbdmeta},
			{"drbd-proxy-ctl", &drbd_proxy_ctl},
			{NULL, NULL}
		};
		size_t len_dir, l;
		struct cmd_helper *c;

		++progname;
		len_dir = progname - argv[0];

		for (c = helpers; c->name; ++c) {
			l = len_dir + strlen(c->name) + 1;
			*(c->var) = malloc(l);
			if (*(c->var)) {
				strncpy(*(c->var), argv[0], len_dir);
				strcpy(*(c->var) + len_dir, c->name);
			}
		}

		/* for pretty printing, truncate to basename */
		argv[0] = progname;
	}
}

int parse_options(int argc, char **argv)
{
	opterr = 1;
	optind = 0;
	while (1) {
		int c;

		c = getopt_long(argc, argv,
				make_optstring(admopt, 0), admopt, 0);
		if (c == -1)
			break;
		switch (c) {
		case 'S':
			is_drbd_top = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'd':
			dry_run++;
			break;
		case 'c':
			if (!strcmp(optarg, "-")) {
				yyin = stdin;
				if (asprintf(&config_file, "STDIN") < 0) {
					fprintf(stderr,
						"asprintf(config_file): %m\n");
					return 20;
				}
				config_from_stdin = 1;
			} else {
				yyin = fopen(optarg, "r");
				if (!yyin) {
					fprintf(stderr, "Can not open '%s'.\n.",
						optarg);
					exit(E_exec_error);
				}
				if (asprintf(&config_file, "%s", optarg) < 0) {
					fprintf(stderr,
						"asprintf(config_file): %m\n");
					return 20;
				}
			}
			break;
		case 's':
			{
				char *pathes[2];
				pathes[0] = optarg;
				pathes[1] = 0;
				find_drbdcmd(&drbdsetup, pathes);
			}
			break;
		case 'm':
			{
				char *pathes[2];
				pathes[0] = optarg;
				pathes[1] = 0;
				find_drbdcmd(&drbdmeta, pathes);
			}
			break;
		case 'p':
			{
				char *pathes[2];
				pathes[0] = optarg;
				pathes[1] = 0;
				find_drbdcmd(&drbd_proxy_ctl, pathes);
			}
			break;
		case 'n':
			{
				char *c;
				int shell_var_name_ok = 1;
				for (c = optarg; *c && shell_var_name_ok; c++) {
					switch (*c) {
					case 'a'...'z':
					case 'A'...'Z':
					case '0'...'9':
					case '_':
						break;
					default:
						shell_var_name_ok = 0;
					}
				}
				if (shell_var_name_ok)
					sh_varname = optarg;
				else
					fprintf(stderr,
						"ignored --sh-varname=%s: "
						"contains suspect characters, allowed set is [a-zA-Z0-9_]\n",
						optarg);
			}
			break;
		case 'f':
			force = 1;
			break;
		case 'V':
			printf("DRBDADM_BUILDTAG=%s\n", shell_escape(drbd_buildtag()));
			printf("DRBDADM_API_VERSION=%u\n", API_VERSION);
			printf("DRBD_KERNEL_VERSION_CODE=0x%06x\n", version_code_kernel());
			printf("DRBDADM_VERSION_CODE=0x%06x\n", version_code_userland());
			printf("DRBDADM_VERSION=%s\n", shell_escape(REL_VERSION));
			exit(0);
			break;
		case 'P':
			connect_to_host = optarg;
			break;
		case '?':
			/* commented out, since opterr=1
			 * fprintf(stderr,"Unknown option %s\n",argv[optind-1]); */
			fprintf(stderr, "try '%s help'\n", progname);
			return 20;
			break;
		}
	}
	return 0;
}

static void substitute_deprecated_cmd(char **c, char *deprecated,
				      char *substitution)
{
	if (!strcmp(*c, deprecated)) {
		fprintf(stderr, "'%s %s' is deprecated, use '%s %s' instead.\n",
			progname, deprecated, progname, substitution);
		*c = substitution;
	}
}

struct adm_cmd *find_cmd(char *cmdname)
{
	struct adm_cmd *cmd = NULL;
	unsigned int i;
	if (!strcmp("hidden-commands", cmdname)) {
		// before parsing the configuration file...
		hidden_cmds(NULL, NULL);
		exit(0);
	}
	if (!strncmp("help", cmdname, 5))
		print_usage_and_exit(0);

	/* R_PRIMARY / R_SECONDARY is not a state, but a role.  Whatever that
	 * means, actually.  But anyways, we decided to start using _role_ as
	 * the terminus of choice, and deprecate "state". */
	substitute_deprecated_cmd(&cmdname, "state", "role");

	/* "outdate-peer" got renamed to fence-peer,
	 * it is not required to actually outdate the peer,
	 * depending on situation it may be sufficient to power-reset it
	 * or do some other fencing action, or even call out to "meatware".
	 * The name of the handler should not imply something that is not done. */
	substitute_deprecated_cmd(&cmdname, "outdate-peer", "fence-peer");

	for (i = 0; i < ARRY_SIZE(cmds); i++) {
		if (!strcmp(cmds[i].name, cmdname)) {
			cmd = cmds + i;
			break;
		}
	}
	return cmd;
}

char *config_file_from_arg(char *arg)
{
	char *f;
	int minor = minor_by_id(arg);

	if (minor < 0) {
		/* this is expected, if someone wants to test the configured
		 * handlers from the command line, using resource names */
		fprintf(stderr,
			"Couldn't find minor from id %s, "
			"expecting minor-<minor> as id. "
			"Trying default config files.\n", arg);
		return NULL;
	}

	f = lookup_minor(minor);
	if (!f) {
		fprintf(stderr,
			"Don't know which config file belongs to minor %d, "
			"trying default ones...\n", minor);
	} else {
		yyin = fopen(f, "r");
		if (yyin == NULL) {
			fprintf(stderr,
				"Couldn't open file %s for reading, reason: %m\n"
				"trying default config file...\n", config_file);
		}
	}
	return f;
}

void assign_default_config_file(void)
{
	int i;
	for (i = 0; conf_file[i]; i++) {
		yyin = fopen(conf_file[i], "r");
		if (yyin) {
			config_file = conf_file[i];
			break;
		}
	}
	if (!config_file) {
		fprintf(stderr, "Can not open '%s': %m\n", conf_file[i - 1]);
		exit(E_config_invalid);
	}
}

void count_resources_or_die(void)
{
	int m, mc = global_options.minor_count;
	struct d_resource *res, *tmp;

	highest_minor = 0;
	for_each_resource(res, tmp, config) {
		if (res->ignore)
			continue;

		m = res->me->device_minor;
		if (m > highest_minor)
			highest_minor = m;
		nr_resources++;
		if (res->stacked)
			nr_stacked++;
		else if (res->ignore)
			nr_ignore++;
		else
			nr_normal++;
	}

	// Just for the case that minor_of_res() returned 0 for all devices.
	if (nr_resources > (highest_minor + 1))
		highest_minor = nr_resources - 1;

	if (mc && mc < (highest_minor + 1)) {
		fprintf(stderr,
			"The highest minor you have in your config is %d"
			"but a minor_count of %d in your config!\n",
			highest_minor, mc);
		exit(E_usage);
	}
}

void die_if_no_resources(void)
{
	if (!is_drbd_top && nr_ignore > 0 && nr_normal == 0) {
		fprintf(stderr,
			"WARN: no normal resources defined for this host (%s)!?\n"
			"Misspelled name of the local machine with the 'on' keyword ?\n",
			nodeinfo.nodename);
		exit(E_usage);
	}
	if (!is_drbd_top && nr_normal == 0) {
		fprintf(stderr,
			"WARN: no normal resources defined for this host (%s)!?\n",
			nodeinfo.nodename);
		exit(E_usage);
	}
	if (is_drbd_top && nr_stacked == 0) {
		fprintf(stderr, "WARN: nothing stacked for this host (%s), "
			"nothing to do in stacked mode!\n", nodeinfo.nodename);
		exit(E_usage);
	}
}

void print_dump_xml_header(void)
{
	printf("<config file=\"%s\">\n", config_save);
	++indent;
	dump_global_info_xml();
	dump_common_info_xml();
}

void print_dump_header(void)
{
	printf("# %s\n", config_save);
	dump_global_info();
	dump_common_info();
}

int main(int argc, char **argv)
{
	size_t i;
	int rv = 0;
	struct adm_cmd *cmd = NULL;
	struct d_resource *res, *tmp;
	char *env_drbd_nodename = NULL;
	int is_dump_xml;
	int is_dump;

	yyin = NULL;
	uname(&nodeinfo);	/* FIXME maybe fold to lower case ? */
	no_tty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	env_drbd_nodename = getenv("__DRBD_NODE__");
	if (env_drbd_nodename && *env_drbd_nodename) {
		strncpy(nodeinfo.nodename, env_drbd_nodename,
			sizeof(nodeinfo.nodename) - 1);
		nodeinfo.nodename[sizeof(nodeinfo.nodename) - 1] = 0;
		fprintf(stderr, "\n"
			"   found __DRBD_NODE__ in environment\n"
			"   PRETENDING that I am >>%s<<\n\n",
			nodeinfo.nodename);
	}

	assign_command_names_from_argv0(argv);

	if (argc == 1)
		print_usage_and_exit("missing arguments");	// arguments missing.

	if (drbdsetup == NULL || drbdmeta == NULL || drbd_proxy_ctl == NULL) {
		fprintf(stderr, "could not strdup argv[0].\n");
		exit(E_exec_error);
	}

	if (!getenv("DRBD_DONT_WARN_ON_VERSION_MISMATCH"))
		warn_on_version_mismatch();

	rv = parse_options(argc, argv);
	if (rv)
		return rv;

	/* store everything before the command name as pass through option/argument */
	while (optind < argc) {
		cmd = find_cmd(argv[optind]);
		if (cmd)
			break;
		setup_opts[soi++] = argv[optind++];
	}

	if (optind == argc)
		print_usage_and_exit(0);

	if (cmd == NULL) {
		fprintf(stderr, "Unknown command '%s'.\n", argv[optind]);
		exit(E_usage);
	}
	do_verify_ips = cmd->verify_ips;
	optind++;

	is_dump_xml = (cmd->function == adm_dump_xml);
	is_dump = (is_dump_xml || cmd->function == adm_dump);

	/* remaining argv are expected to be resource names
	 * optind     == argc: no resourcenames given.
	 * optind + 1 == argc: exactly one resource name (or "all") given
	 * optind + 1  < argc: multiple resource names given. */
	if (optind == argc) {
		if (is_dump)
			all_resources = 1;
		else if (cmd->res_name_required)
			print_usage_and_exit("missing resourcename arguments");
	} else if (optind + 1 < argc) {
		if (!cmd->res_name_required)
			fprintf(stderr,
				"this command will ignore resource names!\n");
		else if (cmd->use_cached_config_file)
			fprintf(stderr,
				"You should not use this command with multiple resources!\n");
	}

	if (!config_file && cmd->use_cached_config_file)
		config_file = config_file_from_arg(argv[optind]);

	if (!config_file)
		/* may exit if no config file can be used! */
		assign_default_config_file();

	/* for error-reporting reasons config_file may be re-assigned by adm_adjust,
	 * we need the current value for register_minor, though.
	 * save that. */
	if (config_from_stdin)
		config_save = config_file;
	else
		config_save = canonify_path(config_file);

	my_parse();

	if (!config_valid)
		exit(E_config_invalid);

	post_parse(config, cmd->is_proxy_cmd ? match_on_proxy : 0);

	if (!is_dump || dry_run || verbose)
		expand_common();
	if (is_dump || dry_run || config_from_stdin)
		do_register_minor = 0;

	count_resources_or_die();

	if (cmd->uc_dialog)
		uc_node(global_options.usage_count);

	if (cmd->res_name_required) {
		if (config == NULL) {
			fprintf(stderr, "no resources defined!\n");
			exit(E_usage);
		}

		global_validate_maybe_expand_die_if_invalid(!is_dump);

		if (optind == argc || !strcmp(argv[optind], "all")) {
			/* either no resource arguments at all,
			 * but command is dump / dump-xml, so implicit "all",
			 * or an explicit "all" argument is given */
			all_resources = 1;
			if (!is_dump || !force)
				die_if_no_resources();
			/* verify ips first, for all of them */
			for_each_resource(res, tmp, config) {
				verify_ips(res);
			}
			if (!config_valid)
				exit(E_config_invalid);

			if (is_dump_xml)
				print_dump_xml_header();
			else if (is_dump)
				print_dump_header();

			for_each_resource(res, tmp, config) {
				if (!is_dump && res->ignore)
					continue;

				if (!is_dump && is_drbd_top != res->stacked)
					continue;
				int r = call_cmd(cmd, res, EXIT_ON_FAIL);	/* does exit for r >= 20! */
				/* this super positioning of return values is soo ugly
				 * anyone any better idea? */
				if (r > rv)
					rv = r;
			}
			if (is_dump_xml) {
				--indent;
				printf("</config>\n");
			}
		} else {
			/* explicit list of resources to work on */
			for (i = optind; (int)i < argc; i++) {
				res = res_by_name(argv[i]);
				if (!res)
					res = res_by_minor(argv[i]);
				if (!res) {
					fprintf(stderr,
						"'%s' not defined in your config.\n",
						argv[i]);
					exit(E_usage);
				}
				if (res->ignore && !is_dump) {
					fprintf(stderr,
						"'%s' ignored, since this host (%s) is not mentioned with an 'on' keyword.\n",
						res->name, nodeinfo.nodename);
					rv = E_usage;
					continue;
				}
				if (is_drbd_top != res->stacked && !is_dump) {
					fprintf(stderr,
						"'%s' is a %s resource, and not available in %s mode.\n",
						res->name,
						res->
						stacked ? "stacked" : "normal",
						is_drbd_top ? "stacked" :
						"normal");
					rv = E_usage;
					continue;
				}
				verify_ips(res);
				if (!is_dump && !config_valid)
					exit(E_config_invalid);
				rv = call_cmd(cmd, res, EXIT_ON_FAIL);	/* does exit for rv >= 20! */
			}
		}
	} else {		// Commands which do not need a resource name
		/* no call_cmd, as that implies register_minor,
		 * which does not make sense for resource independent commands */
		rv = cmd->function(config, cmd->name);
		if (rv >= 10) {	/* why do we special case the "generic sh-*" commands? */
			fprintf(stderr, "command %s exited with code %d\n",
				cmd->name, rv);
			exit(rv);
		}
	}

	/* do we really have to bitor the exit code?
	 * it is even only a Boolean value in this case! */
	rv |= run_dcmds();

	free_config(config);

	return rv;
}

void yyerror(char *text)
{
	fprintf(stderr, "%s:%d: %s\n", config_file, line, text);
	exit(E_syntax);
}
