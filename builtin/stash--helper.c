#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "refs.h"
#include "lockfile.h"
#include "cache-tree.h"
#include "unpack-trees.h"
#include "merge-recursive.h"
#include "argv-array.h"
#include "run-command.h"
#include "dir.h"
#include "rerere.h"
#include "revision.h"
#include "log-tree.h"

static const char * const git_stash_helper_usage[] = {
	N_("git stash--helper list [<options>]"),
	N_("git stash--helper show [<options>] [<stash>]"),
	N_("git stash--helper drop [-q|--quiet] [<stash>]"),
	N_("git stash--helper ( pop | apply ) [--index] [-q|--quiet] [<stash>]"),
	N_("git stash--helper branch <branchname> [<stash>]"),
	N_("git stash--helper clear"),
	N_("git stash--helper store [-m|--message <message>] [-q|--quiet] <commit>"),
	N_("git stash--helper create [<message>]"),
	NULL
};

static const char * const git_stash_helper_list_usage[] = {
	N_("git stash--helper list [<options>]"),
	NULL
};

static const char * const git_stash_helper_show_usage[] = {
	N_("git stash--helper show [<options>] [<stash>]"),
	NULL
};

static const char * const git_stash_helper_drop_usage[] = {
	N_("git stash--helper drop [-q|--quiet] [<stash>]"),
	NULL
};

static const char * const git_stash_helper_pop_usage[] = {
	N_("git stash--helper pop [--index] [-q|--quiet] [<stash>]"),
	NULL
};

static const char * const git_stash_helper_apply_usage[] = {
	N_("git stash--helper apply [--index] [-q|--quiet] [<stash>]"),
	NULL
};

static const char * const git_stash_helper_branch_usage[] = {
	N_("git stash--helper branch <branchname> [<stash>]"),
	NULL
};

static const char * const git_stash_helper_clear_usage[] = {
	N_("git stash--helper clear"),
	NULL
};

static const char * const git_stash_helper_store_usage[] = {
	N_("git stash--helper store [-m|--message <message>] [-q|--quiet] <commit>"),
	NULL
};

static const char * const git_stash_helper_create_usage[] = {
	N_("git stash--helper create [<message>]"),
	NULL
};

static const char *ref_stash = "refs/stash";
static int quiet;
static struct strbuf stash_index_path = STRBUF_INIT;

/*
 * w_commit is set to the commit containing the working tree
 * b_commit is set to the base commit
 * i_commit is set to the commit containing the index tree
 * u_commit is set to the commit containing the untracked files tree
 * w_tree is set to the working tree
 * b_tree is set to the base tree
 * i_tree is set to the index tree
 * u_tree is set to the untracked files tree
 */

struct stash_info {
	struct object_id w_commit;
	struct object_id b_commit;
	struct object_id i_commit;
	struct object_id u_commit;
	struct object_id w_tree;
	struct object_id b_tree;
	struct object_id i_tree;
	struct object_id u_tree;
	struct strbuf revision;
	int is_stash_ref;
	int has_u;
};

static void free_stash_info(struct stash_info *info)
{
	strbuf_release(&info->revision);
}

static void assert_stash_like(struct stash_info *info, const char * revision)
{
	if (get_oidf(&info->b_commit, "%s^1", revision) ||
	    get_oidf(&info->w_tree, "%s:", revision) ||
	    get_oidf(&info->b_tree, "%s^1:", revision) ||
	    get_oidf(&info->i_tree, "%s^2:", revision)) {
		free_stash_info(info);
		error(_("'%s' is not a stash-like commit"), revision);
		exit(128);
	}
}

static int get_stash_info(struct stash_info *info, int argc, const char **argv)
{
	struct strbuf symbolic = STRBUF_INIT;
	int ret;
	const char *revision;
	const char *commit = NULL;
	char *end_of_rev;
	char *expanded_ref;
	struct object_id dummy;

	if (argc > 1) {
		int i;
		struct strbuf refs_msg = STRBUF_INIT;
		for (i = 0; i < argc; ++i)
			strbuf_addf(&refs_msg, " '%s'", argv[i]);

		fprintf_ln(stderr, _("Too many revisions specified:%s"),
			   refs_msg.buf);
		strbuf_release(&refs_msg);

		return -1;
	}

	if (argc == 1)
		commit = argv[0];

	strbuf_init(&info->revision, 0);
	if (!commit) {
		if (!ref_exists(ref_stash)) {
			free_stash_info(info);
			fprintf_ln(stderr, "No stash entries found.");
			return -1;
		}

		strbuf_addf(&info->revision, "%s@{0}", ref_stash);
	} else if (strspn(commit, "0123456789") == strlen(commit)) {
		strbuf_addf(&info->revision, "%s@{%s}", ref_stash, commit);
	} else {
		strbuf_addstr(&info->revision, commit);
	}

	revision = info->revision.buf;

	if (get_oid(revision, &info->w_commit)) {
		error(_("%s is not a valid reference"), revision);
		free_stash_info(info);
		return -1;
	}

	assert_stash_like(info, revision);

	info->has_u = !get_oidf(&info->u_tree, "%s^3:", revision);

	end_of_rev = strchrnul(revision, '@');
	strbuf_add(&symbolic, revision, end_of_rev - revision);

	ret = dwim_ref(symbolic.buf, symbolic.len, &dummy, &expanded_ref);
	strbuf_release(&symbolic);
	switch (ret) {
	case 0: /* Not found, but valid ref */
		info->is_stash_ref = 0;
		break;
	case 1:
		info->is_stash_ref = !strcmp(expanded_ref, ref_stash);
		break;
	default: /* Invalid or ambiguous */
		free_stash_info(info);
	}

	free(expanded_ref);
	return !(ret == 0 || ret == 1);
}

static int do_clear_stash(void)
{
	struct object_id obj;
	if (get_oid(ref_stash, &obj))
		return 0;

	return delete_ref(NULL, ref_stash, &obj, 0);
}

static int clear_stash(int argc, const char **argv, const char *prefix)
{
	struct option options[] = {
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_clear_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);

	if (argc != 0)
		return error(_("git stash--helper clear with parameters is unimplemented"));

	return do_clear_stash();
}

static int reset_tree(struct object_id *i_tree, int update, int reset)
{
	struct unpack_trees_options opts;
	int nr_trees = 1;
	struct tree_desc t[MAX_UNPACK_TREES];
	struct tree *tree;
	struct lock_file lock_file = LOCK_INIT;

	read_cache_preload(NULL);
	if (refresh_cache(REFRESH_QUIET))
		return -1;

	hold_locked_index(&lock_file, LOCK_DIE_ON_ERROR);

	memset(&opts, 0, sizeof(opts));

	tree = parse_tree_indirect(i_tree);
	if (parse_tree(tree))
		return -1;

	init_tree_desc(t, tree->buffer, tree->size);

	opts.head_idx = 1;
	opts.src_index = &the_index;
	opts.dst_index = &the_index;
	opts.merge = 1;
	opts.reset = reset;
	opts.update = update;
	opts.fn = oneway_merge;

	if (unpack_trees(nr_trees, t, &opts))
		return -1;

	if (write_locked_index(&the_index, &lock_file, COMMIT_LOCK))
		return error(_("unable to write new index file"));

	return 0;
}

static int diff_tree_binary(struct strbuf *out, struct object_id *w_commit)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *w_commit_hex = oid_to_hex(w_commit);

	/*
	 * Diff-tree would not be very hard to replace with a native function,
	 * however it should be done together with apply_cached.
	 */
	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "diff-tree", "--binary", NULL);
	argv_array_pushf(&cp.args, "%s^2^..%s^2", w_commit_hex, w_commit_hex);

	return pipe_command(&cp, NULL, 0, out, 0, NULL, 0);
}

static int apply_cached(struct strbuf *out)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	/*
	 * Apply currently only reads either from stdin or a file, thus
	 * apply_all_patches would have to be updated to optionally take a
	 * buffer.
	 */
	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "apply", "--cached", NULL);
	return pipe_command(&cp, out->buf, out->len, NULL, 0, NULL, 0);
}

static int reset_head(const char *prefix)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	/*
	 * Reset is overall quite simple, however there is no current public
	 * API for resetting.
	 */
	cp.git_cmd = 1;
	argv_array_push(&cp.args, "reset");

	return run_command(&cp);
}

static int get_newly_staged(struct strbuf *out, struct object_id *c_tree)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *c_tree_hex = oid_to_hex(c_tree);

	/*
	 * diff-index is very similar to diff-tree above, and should be
	 * converted together with update_index.
	 */
	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "diff-index", "--cached", "--name-only",
			 "--diff-filter=A", NULL);
	argv_array_push(&cp.args, c_tree_hex);
	return pipe_command(&cp, NULL, 0, out, 0, NULL, 0);
}

static int update_index(struct strbuf *out)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	/*
	 * Update-index is very complicated and may need to have a public
	 * function exposed in order to remove this forking.
	 */
	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "update-index", "--add", "--stdin", NULL);
	return pipe_command(&cp, out->buf, out->len, NULL, 0, NULL, 0);
}

static int restore_untracked(struct object_id *u_tree)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	int res;

	/*
	 * We need to run restore files from a given index, but without
	 * affecting the current index, so we use GIT_INDEX_FILE with
	 * run_command to fork processes that will not interfere.
	 */
	cp.git_cmd = 1;
	argv_array_push(&cp.args, "read-tree");
	argv_array_push(&cp.args, oid_to_hex(u_tree));
	argv_array_pushf(&cp.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);
	if (run_command(&cp)) {
		remove_path(stash_index_path.buf);
		return -1;
	}

	child_process_init(&cp);
	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "checkout-index", "--all", NULL);
	argv_array_pushf(&cp.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);

	res = run_command(&cp);
	remove_path(stash_index_path.buf);
	return res;
}

static int do_apply_stash(const char *prefix, struct stash_info *info,
	int index)
{
	struct merge_options o;
	struct object_id c_tree;
	struct object_id index_tree;
	const struct object_id *bases[1];
	struct commit *result;
	int ret;
	int has_index = index;

	read_cache_preload(NULL);
	if (refresh_cache(REFRESH_QUIET))
		return -1;

	if (write_cache_as_tree(&c_tree, 0, NULL) || reset_tree(&c_tree, 0, 0))
		return error(_("Cannot apply a stash in the middle of a merge"));

	if (index) {
		if (!oidcmp(&info->b_tree, &info->i_tree) || !oidcmp(&c_tree,
			&info->i_tree)) {
			has_index = 0;
		} else {
			struct strbuf out = STRBUF_INIT;

			if (diff_tree_binary(&out, &info->w_commit)) {
				strbuf_release(&out);
				return -1;
			}

			ret = apply_cached(&out);
			strbuf_release(&out);
			if (ret)
				return -1;

			discard_cache();
			read_cache();
			if (write_cache_as_tree(&index_tree, 0, NULL))
				return -1;

			reset_head(prefix);
		}
	}

	if (info->has_u && restore_untracked(&info->u_tree))
		return error(_("Could not restore untracked files from stash"));

	init_merge_options(&o);

	o.branch1 = "Updated upstream";
	o.branch2 = "Stashed changes";

	if (!oidcmp(&info->b_tree, &c_tree))
		o.branch1 = "Version stash was based on";

	if (quiet)
		o.verbosity = 0;

	if (o.verbosity >= 3)
		printf_ln(_("Merging %s with %s"), o.branch1, o.branch2);

	bases[0] = &info->b_tree;

	ret = merge_recursive_generic(&o, &c_tree, &info->w_tree, 1, bases,
				      &result);
	if (ret != 0) {
		rerere(0);

		if (index)
			fprintf_ln(stderr, _("Index was not unstashed."));

		return ret;
	}

	if (has_index) {
		if (reset_tree(&index_tree, 0, 0))
			return -1;
	} else {
		struct strbuf out = STRBUF_INIT;

		if (get_newly_staged(&out, &c_tree)) {
			strbuf_release(&out);
			return -1;
		}

		if (reset_tree(&c_tree, 0, 1)) {
			strbuf_release(&out);
			return -1;
		}

		ret = update_index(&out);
		strbuf_release(&out);
		if (ret)
			return -1;

		discard_cache();
	}

	if (quiet) {
		if (refresh_cache(REFRESH_QUIET))
			warning("could not refresh index");
	} else {
		struct child_process cp = CHILD_PROCESS_INIT;

		/*
		 * Status is quite simple and could be replaced with calls to
		 * wt_status in the future, but it adds complexities which may
		 * require more tests.
		 */
		cp.git_cmd = 1;
		cp.dir = prefix;
		argv_array_push(&cp.args, "status");
		run_command(&cp);
	}

	return 0;
}

static int apply_stash(int argc, const char **argv, const char *prefix)
{
	int index = 0;
	struct stash_info info;
	int ret;
	struct option options[] = {
		OPT__QUIET(&quiet, N_("be quiet, only report errors")),
		OPT_BOOL(0, "index", &index,
			N_("attempt to recreate the index")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_apply_usage, 0);

	if (get_stash_info(&info, argc, argv))
		return -1;

	ret = do_apply_stash(prefix, &info, index);
	free_stash_info(&info);
	return ret;
}

static int do_drop_stash(const char *prefix, struct stash_info *info)
{
	struct child_process cp_reflog = CHILD_PROCESS_INIT;
	struct child_process cp = CHILD_PROCESS_INIT;
	int ret;

	/*
	 * reflog does not provide a simple function for deleting refs. One will
	 * need to be added to avoid implementing too much reflog code here
	 */

	cp_reflog.git_cmd = 1;
	argv_array_pushl(&cp_reflog.args, "reflog", "delete", "--updateref",
			 "--rewrite", NULL);
	argv_array_push(&cp_reflog.args, info->revision.buf);
	ret = run_command(&cp_reflog);
	if (!ret) {
		if (!quiet)
			printf(_("Dropped %s (%s)\n"), info->revision.buf,
			       oid_to_hex(&info->w_commit));
	} else {
		return error(_("%s: Could not drop stash entry"),
			     info->revision.buf);
	}

	/*
	 * This could easily be replaced by get_oid, but currently it will throw
	 * a fatal error when a reflog is empty, which we can not recover from.
	 */
	cp.git_cmd = 1;
	/* Even though --quiet is specified, rev-parse still outputs the hash */
	cp.no_stdout = 1;
	argv_array_pushl(&cp.args, "rev-parse", "--verify", "--quiet", NULL);
	argv_array_pushf(&cp.args, "%s@{0}", ref_stash);
	ret = run_command(&cp);

	/* do_clear_stash if we just dropped the last stash entry */
	if (ret)
		do_clear_stash();

	return 0;
}

static void assert_stash_ref(struct stash_info *info)
{
	if (!info->is_stash_ref) {
		free_stash_info(info);
		error(_("'%s' is not a stash reference"), info->revision.buf);
		exit(128);
	}
}

static int drop_stash(int argc, const char **argv, const char *prefix)
{
	struct stash_info info;
	int ret;
	struct option options[] = {
		OPT__QUIET(&quiet, N_("be quiet, only report errors")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_drop_usage, 0);

	if (get_stash_info(&info, argc, argv))
		return -1;

	assert_stash_ref(&info);

	ret = do_drop_stash(prefix, &info);
	free_stash_info(&info);
	return ret;
}

static int pop_stash(int argc, const char **argv, const char *prefix)
{
	int index = 0, ret;
	struct stash_info info;
	struct option options[] = {
		OPT__QUIET(&quiet, N_("be quiet, only report errors")),
		OPT_BOOL(0, "index", &index,
			N_("attempt to recreate the index")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_pop_usage, 0);

	if (get_stash_info(&info, argc, argv))
		return -1;

	assert_stash_ref(&info);
	if ((ret = do_apply_stash(prefix, &info, index)))
		printf_ln(_("The stash entry is kept in case you need it again."));
	else
		ret = do_drop_stash(prefix, &info);

	free_stash_info(&info);
	return ret;
}

static int branch_stash(int argc, const char **argv, const char *prefix)
{
	const char *branch = NULL;
	int ret;
	struct child_process cp = CHILD_PROCESS_INIT;
	struct stash_info info;
	struct option options[] = {
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_branch_usage, 0);

	if (argc == 0)
		return error(_("No branch name specified"));

	branch = argv[0];

	if (get_stash_info(&info, argc - 1, argv + 1))
		return -1;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "checkout", "-b", NULL);
	argv_array_push(&cp.args, branch);
	argv_array_push(&cp.args, oid_to_hex(&info.b_commit));
	ret = run_command(&cp);
	if (!ret)
		ret = do_apply_stash(prefix, &info, 1);
	if (!ret && info.is_stash_ref)
		ret = do_drop_stash(prefix, &info);

	free_stash_info(&info);

	return ret;
}

static int list_stash(int argc, const char **argv, const char *prefix)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct option options[] = {
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_list_usage,
			     PARSE_OPT_KEEP_UNKNOWN);

	if (!ref_exists(ref_stash))
		return 0;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "log", "--format=%gd: %gs", "-g",
			 "--first-parent", "-m", NULL);
	argv_array_pushv(&cp.args, argv);
	argv_array_push(&cp.args, ref_stash);
	argv_array_push(&cp.args, "--");
	return run_command(&cp);
}

static int show_stat = 1;
static int show_patch;

static int git_stash_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, "stash.showStat")) {
		show_stat = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "stash.showPatch")) {
		show_patch = git_config_bool(var, value);
		return 0;
	}
	return git_default_config(var, value, cb);
}

static int show_stash(int argc, const char **argv, const char *prefix)
{
	int i;
	int flags = 0;
	struct stash_info info;
	struct rev_info rev;
	struct argv_array stash_args = ARGV_ARRAY_INIT;
	struct option options[] = {
		OPT_END()
	};

	init_diff_ui_defaults();
	git_config(git_diff_ui_config, NULL);

	init_revisions(&rev, prefix);

	/* Push arguments which are not options into stash_args. */
	for (i = 1; i < argc; ++i) {
		if (argv[i][0] != '-')
			argv_array_push(&stash_args, argv[i]);
		else
			flags++;
	}

	/*
	 * The config settings are applied only if there are not passed
	 * any flags.
	 */
	if (!flags) {
		git_config(git_stash_config, NULL);
		if (show_stat)
			rev.diffopt.output_format |= DIFF_FORMAT_DIFFSTAT;
		if (show_patch) {
			rev.diffopt.output_format = ~DIFF_FORMAT_NO_OUTPUT;
			rev.diffopt.output_format |= DIFF_FORMAT_PATCH;
		}
	}

	if (get_stash_info(&info, stash_args.argc, stash_args.argv)) {
		argv_array_clear(&stash_args);
		return -1;
	}

	argc = setup_revisions(argc, argv, &rev, NULL);
	if (!rev.diffopt.output_format)
		rev.diffopt.output_format = DIFF_FORMAT_PATCH;
	diff_setup_done(&rev.diffopt);
	rev.diffopt.flags.recursive = 1;
	setup_diff_pager(&rev.diffopt);

	/*
	 * We can return early if there was any option not recognised by
	 * `diff_opt_parse()`, besides the word `stash`.
	 */
	if (argc > 1) {
		free_stash_info(&info);
		argv_array_clear(&stash_args);
		usage_with_options(git_stash_helper_show_usage, options);
	}

	/* Do the diff thing. */
	diff_tree_oid(&info.b_commit, &info.w_commit, "", &rev.diffopt);
	log_tree_diff_flush(&rev);

	free_stash_info(&info);
	argv_array_clear(&stash_args);
	return 0;
}

static int do_store_stash(const char *w_commit, const char *stash_msg,
			  int quiet)
{
	int ret = 0;
	struct object_id obj;

	if (!stash_msg)
		stash_msg  = xstrdup("Created via \"git stash--helper store\".");

	ret = get_oid(w_commit, &obj);
	if (!ret) {
		ret = update_ref(stash_msg, ref_stash, &obj, NULL,
				 REF_FORCE_CREATE_REFLOG,
				 quiet ? UPDATE_REFS_QUIET_ON_ERR :
				 UPDATE_REFS_MSG_ON_ERR);
	}
	if (ret && !quiet)
		fprintf_ln(stderr, _("Cannot update %s with %s"),
			   ref_stash, w_commit);

	return ret;
}

static int store_stash(int argc, const char **argv, const char *prefix)
{
	const char *stash_msg = NULL;
	struct option options[] = {
		OPT__QUIET(&quiet, N_("be quiet, only report errors")),
		OPT_STRING('m', "message", &stash_msg, "message", N_("stash message")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_store_usage,
			     PARSE_OPT_KEEP_UNKNOWN);

	if (argc != 1) {
		fprintf(stderr, _("\"git stash--helper store\" requires one <commit> argument\n"));
		return -1;
	}

	return do_store_stash(argv[0], stash_msg, quiet);
}

/*
 * `out` will be filled with the names of untracked files. The return value is:
 *
 * < 0 if there was a bug (any arg given outside the repo will be detected
 *     by `setup_revision()`)
 * = 0 if there are not any untracked files
 * > 0 if there are untracked files
 */
static int get_untracked_files(const char **argv, int line_term,
			       int include_untracked, struct strbuf *out)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "ls-files", "-o", NULL);
	if (line_term)
		argv_array_push(&cp.args, "-z");
	if (include_untracked != 2)
		argv_array_push(&cp.args, "--exclude-standard");
	argv_array_push(&cp.args, "--");
	if (argv)
		argv_array_pushv(&cp.args, argv);

	if (pipe_command(&cp, NULL, 0, out, 0, NULL, 0))
		return -1;
	return out->len;
}

/*
 * The return value of `check_changes()` can be:
 *
 * < 0 if there was an error
 * = 0 if there are no changes.
 * > 0 if there are changes.
 */
static int check_changes(const char **argv, int include_untracked,
			 const char *prefix)
{
	int result;
	int ret = 0;
	struct rev_info rev;
	struct object_id dummy;
	struct strbuf out = STRBUF_INIT;
	struct argv_array args = ARGV_ARRAY_INIT;

	init_revisions(&rev, prefix);
	parse_pathspec(&rev.prune_data, 0, PATHSPEC_PREFER_FULL,
		       prefix, argv);

	rev.diffopt.flags.quick = 1;
	rev.diffopt.flags.ignore_submodules = 1;
	rev.abbrev = 0;

	/* No initial commit. */
	if (get_oid("HEAD", &dummy)) {
		ret = -1;
		goto done;
	}

	add_head_to_pending(&rev);
	diff_setup_done(&rev.diffopt);

	if (read_cache() < 0) {
		ret = -1;
		goto done;
	}
	result = run_diff_index(&rev, 1);
	if (diff_result_code(&rev.diffopt, result)) {
		ret = 1;
		goto done;
	}

	object_array_clear(&rev.pending);
	result = run_diff_files(&rev, 0);
	if (diff_result_code(&rev.diffopt, result)) {
		ret = 1;
		goto done;
	}

	if (include_untracked && get_untracked_files(argv, 0,
						     include_untracked, &out))
		ret = 1;

done:
	strbuf_release(&out);
	argv_array_clear(&args);
	return ret;
}

static int save_untracked_files(struct stash_info *info, struct strbuf *msg,
				struct strbuf *out)
{
	int ret = 0;
	struct strbuf untracked_msg = STRBUF_INIT;
	struct strbuf out2 = STRBUF_INIT;
	struct child_process cp = CHILD_PROCESS_INIT;
	struct child_process cp2 = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "update-index", "-z", "--add",
			 "--remove", "--stdin", NULL);
	argv_array_pushf(&cp.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);

	strbuf_addf(&untracked_msg, "untracked files on %s\n", msg->buf);
	if (pipe_command(&cp, out->buf, out->len, NULL, 0, NULL, 0)) {
		ret = -1;
		goto done;
	}

	cp2.git_cmd = 1;
	argv_array_push(&cp2.args, "write-tree");
	argv_array_pushf(&cp2.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);
	if (pipe_command(&cp2, NULL, 0, &out2, 0,NULL, 0)) {
		ret = -1;
		goto done;
	}
	get_oid_hex(out2.buf, &info->u_tree);

	if (commit_tree(untracked_msg.buf, untracked_msg.len,
			&info->u_tree, NULL, &info->u_commit, NULL, NULL)) {
		ret = -1;
		goto done;
	}

done:
	strbuf_release(&untracked_msg);
	strbuf_release(&out2);
	remove_path(stash_index_path.buf);
	return ret;
}

static struct strbuf patch = STRBUF_INIT;

static int stash_patch(struct stash_info *info, const char **argv)
{
	int ret = 0;
	struct strbuf out2 = STRBUF_INIT;
	struct child_process cp0 = CHILD_PROCESS_INIT;
	struct child_process cp1 = CHILD_PROCESS_INIT;
	struct child_process cp2 = CHILD_PROCESS_INIT;
	struct child_process cp3 = CHILD_PROCESS_INIT;

	remove_path(stash_index_path.buf);

	cp0.git_cmd = 1;
	argv_array_pushl(&cp0.args, "read-tree", "HEAD", NULL);
	argv_array_pushf(&cp0.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);
	if (run_command(&cp0)) {
		ret = -1;
		goto done;
	}

	cp1.git_cmd = 1;
	argv_array_pushl(&cp1.args, "add--interactive", "--patch=stash",
			"--", NULL);
	if (argv)
		argv_array_pushv(&cp1.args, argv);
	argv_array_pushf(&cp1.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);
	if (run_command(&cp1)) {
		ret = -1;
		goto done;
	}

	cp2.git_cmd = 1;
	argv_array_push(&cp2.args, "write-tree");
	argv_array_pushf(&cp2.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);
	if (pipe_command(&cp2, NULL, 0, &out2, 0,NULL, 0)) {
		ret = -1;
		goto done;
	}

	get_oid_hex(out2.buf, &info->w_tree);

	cp3.git_cmd = 1;
	argv_array_pushl(&cp3.args, "diff-tree", "-p", "HEAD",
			 oid_to_hex(&info->w_tree), "--", NULL);
	if (pipe_command(&cp3, NULL, 0, &patch, 0, NULL, 0))
		ret = -1;

	if (!patch.len) {
		fprintf_ln(stdout, "No changes selected");
		ret = 1;
	}

done:
	strbuf_release(&out2);
	remove_path(stash_index_path.buf);
	return ret;
}

static int stash_working_tree(struct stash_info *info, const char **argv)
{
	int ret = 0;
	struct child_process cp1 = CHILD_PROCESS_INIT;
	struct child_process cp2 = CHILD_PROCESS_INIT;
	struct child_process cp3 = CHILD_PROCESS_INIT;
	struct strbuf out1 = STRBUF_INIT;
	struct strbuf out3 = STRBUF_INIT;

	set_alternate_index_output(stash_index_path.buf);
	if (reset_tree(&info->i_tree, 0, 0)) {
		ret = -1;
		goto done;
	}
	set_alternate_index_output(".git/index");

	cp1.git_cmd = 1;
	argv_array_pushl(&cp1.args, "diff-index", "--name-only", "-z",
			"HEAD", "--", NULL);
	if (argv)
		argv_array_pushv(&cp1.args, argv);
	argv_array_pushf(&cp1.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);

	if (pipe_command(&cp1, NULL, 0, &out1, 0, NULL, 0)) {
		ret = -1;
		goto done;
	}

	cp2.git_cmd = 1;
	argv_array_pushl(&cp2.args, "update-index", "-z", "--add",
			 "--remove", "--stdin", NULL);
	argv_array_pushf(&cp2.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);

	if (pipe_command(&cp2, out1.buf, out1.len, NULL, 0, NULL, 0)) {
		ret = -1;
		goto done;
	}

	cp3.git_cmd = 1;
	argv_array_push(&cp3.args, "write-tree");
	argv_array_pushf(&cp3.env_array, "GIT_INDEX_FILE=%s",
			 stash_index_path.buf);
	if (pipe_command(&cp3, NULL, 0, &out3, 0,NULL, 0)) {
		ret = -1;
		goto done;
	}

	get_oid_hex(out3.buf, &info->w_tree);

done:
	strbuf_release(&out1);
	strbuf_release(&out3);
	remove_path(stash_index_path.buf);
	return ret;
}

static int do_create_stash(int argc, const char **argv, const char *prefix,
			   const char **stash_msg, int include_untracked,
			   int patch_mode, struct stash_info *info)
{
	int untracked_commit_option = 0;
	int ret = 0;
	int subject_len;
	int flags;
	const char *head_short_sha1 = NULL;
	const char *branch_ref = NULL;
	const char *head_subject = NULL;
	const char *branch_name = "(no branch)";
	struct commit *head_commit = NULL;
	struct commit_list *parents = NULL;
	struct strbuf msg = STRBUF_INIT;
	struct strbuf commit_tree_label = STRBUF_INIT;
	struct strbuf out = STRBUF_INIT;
	struct strbuf final_stash_msg = STRBUF_INIT;

	read_cache_preload(NULL);
	refresh_cache(REFRESH_QUIET);

	if (!check_changes(argv, include_untracked, prefix)) {
		ret = 1;
		goto done;
	}

	if (get_oid("HEAD", &info->b_commit)) {
		fprintf_ln(stderr, "You do not have the initial commit yet");
		ret = -1;
		goto done;
	} else {
		head_commit = lookup_commit(the_repository, &info->b_commit);
	}

	branch_ref = resolve_ref_unsafe("HEAD", 0, NULL, &flags);
	if (flags & REF_ISSYMREF)
		branch_name = strrchr(branch_ref, '/') + 1;
	head_short_sha1 = find_unique_abbrev(&head_commit->object.oid,
					     DEFAULT_ABBREV);
	subject_len = find_commit_subject(get_commit_buffer(head_commit, NULL),
					  &head_subject);
	strbuf_addf(&msg, "%s: %s %.*s\n", branch_name, head_short_sha1,
		    subject_len, head_subject);

	strbuf_addf(&commit_tree_label, "index on %s\n", msg.buf);
	commit_list_insert(head_commit, &parents);
	if (write_cache_as_tree(&info->i_tree, 0, NULL) ||
	    commit_tree(commit_tree_label.buf, commit_tree_label.len,
			&info->i_tree, parents, &info->i_commit, NULL, NULL)) {
		fprintf_ln(stderr, "Cannot save the current index state");
		ret = -1;
		goto done;
	}

	if (include_untracked && get_untracked_files(argv, 1,
						     include_untracked, &out)) {
		if (save_untracked_files(info, &msg, &out)) {
			printf_ln("Cannot save the untracked files");
			ret = -1;
			goto done;
		}
		untracked_commit_option = 1;
	}
	if (patch_mode) {
		ret = stash_patch(info, argv);
		if (ret < 0) {
			printf_ln("Cannot save the current worktree state");
			goto done;
		} else if (ret > 0) {
			goto done;
		}
	} else {
		if (stash_working_tree(info, argv)) {
			printf_ln("Cannot save the current worktree state");
			ret = -1;
			goto done;
		}
	}

	if (!*stash_msg || !strlen(*stash_msg))
		strbuf_addf(&final_stash_msg, "WIP on %s", msg.buf);
	else
		strbuf_addf(&final_stash_msg, "On %s: %s\n", branch_name,
			    *stash_msg);
	*stash_msg = strbuf_detach(&final_stash_msg, NULL);

	/*
	 * `parents` will be empty after calling `commit_tree()`, so there is
	 * no need to call `free_commit_list()`
	 */
	parents = NULL;
	if (untracked_commit_option)
		commit_list_insert(lookup_commit(the_repository, &info->u_commit), &parents);
	commit_list_insert(lookup_commit(the_repository, &info->i_commit), &parents);
	commit_list_insert(head_commit, &parents);

	if (commit_tree(*stash_msg, strlen(*stash_msg), &info->w_tree,
			parents, &info->w_commit, NULL, NULL)) {
		printf_ln("Cannot record working tree state");
		ret = -1;
		goto done;
	}

done:
	strbuf_release(&commit_tree_label);
	strbuf_release(&msg);
	strbuf_release(&out);
	strbuf_release(&final_stash_msg);
	return ret;
}

static int create_stash(int argc, const char **argv, const char *prefix)
{
	int include_untracked = 0;
	int ret = 0;
	const char *stash_msg = NULL;
	struct stash_info info;
	struct option options[] = {
		OPT_BOOL('u', "include-untracked", &include_untracked,
			 N_("include untracked files in stash")),
		OPT_STRING('m', "message", &stash_msg, N_("message"),
			 N_("stash message")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options,
			     git_stash_helper_create_usage,
			     0);

	ret = do_create_stash(argc, argv, prefix, &stash_msg,
			      include_untracked, 0, &info);

	if (!ret)
		printf_ln("%s", oid_to_hex(&info.w_commit));

	/*
	 * ret can be 1 if there were no changes. In this case, we should
	 * not error out.
	 */
	return ret < 0;
}

int cmd_stash__helper(int argc, const char **argv, const char *prefix)
{
	pid_t pid = getpid();
	const char *index_file;

	struct option options[] = {
		OPT_END()
	};

	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, options, git_stash_helper_usage,
			     PARSE_OPT_KEEP_UNKNOWN | PARSE_OPT_KEEP_DASHDASH);

	index_file = get_index_file();
	strbuf_addf(&stash_index_path, "%s.stash.%" PRIuMAX, index_file,
		    (uintmax_t)pid);

	if (argc < 1)
		usage_with_options(git_stash_helper_usage, options);
	if (!strcmp(argv[0], "apply"))
		return !!apply_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "clear"))
		return !!clear_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "drop"))
		return !!drop_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "pop"))
		return !!pop_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "branch"))
		return !!branch_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "list"))
		return !!list_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "show"))
		return !!show_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "store"))
		return !!store_stash(argc, argv, prefix);
	else if (!strcmp(argv[0], "create"))
		return !!create_stash(argc, argv, prefix);

	usage_msg_opt(xstrfmt(_("unknown subcommand: %s"), argv[0]),
		      git_stash_helper_usage, options);
}
