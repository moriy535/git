#!/bin/sh

# This test can give false success if your machine is sufficiently
# slow or all trials happened to happen on second boundaries.

test_description='racy split index'

. ./test-lib.sh

test_expect_success 'setup' '
	# Only split the index when the test explicitly says so.
	sane_unset GIT_TEST_SPLIT_INDEX GIT_FSMONITOR_TEST &&
	git config splitIndex.maxPercentChange 100 &&

	echo something >other-file &&
	# No raciness with this file.
	test-tool chmtime =-20 other-file
'

check_cached_diff () {
	git diff-index --patch --cached $EMPTY_TREE racy-file >diff &&
	tail -1 diff >actual &&
	echo "+cached content" >expect &&
	test_cmp expect actual
}

for trial in 0 1 2 3 4
do
	test_expect_success "split the index when the worktree contains a racily clean file #$trial" '
		test_when_finished "rm -f .git/index .git/sharedindex.*" &&

		# The next three commands must be run within the same
		# second (so both writes to racy-file result in the same
		# mtime) to create the interesting racy situation.
		echo "cached content" >racy-file &&

		# Update and split the index.  The cache entry of
		# racy-file will be stored in the shared index.
		git update-index --split-index --add racy-file &&

		# File size must stay the same.
		echo "dirty worktree" >racy-file &&

		check_cached_diff
	'
done

for trial in 0 1 2 3 4
do
	test_expect_success "split the index when it contains a racily clean cache entry #$trial" '
		test_when_finished "rm -f .git/index .git/sharedindex.*" &&

		# The next three commands must be run within the same
		# second.
		echo "cached content" >racy-file &&

		git update-index --add racy-file &&

		# File size must stay the same.
		echo "dirty worktree" >racy-file &&

		# Now wait a bit to ensure that the split index written
		# below will get a more recent mtime than racy-file,
		# and, consequently, subsequent git commands wont
		# consider the entry racily clean.
		sleep 1 &&

		# Update and split the index when it contains the
		# racily clean cache entry of racy-file; the stat data
		# in that entry should be smudged, so the file wont
		# appear clean for subsequent git commands.
		git update-index --split-index --add other-file &&

		check_cached_diff
	'
done

for trial in 0 1 2 3 4
do
	test_expect_success "update the split index when the shared index contains a racily clean cache entry #$trial" '
		test_when_finished "rm -f .git/index .git/sharedindex.*" &&

		# The next three commands must be run within the same
		# second.
		echo "cached content" >racy-file &&

		# Update and split the index.  The cache entry of
		# racy-file will be stored in the shared index.
		git update-index --split-index --add racy-file &&

		# File size must stay the same.
		echo "dirty worktree" >racy-file &&

		# Now wait a bit to ensure that the split index written
		# below will get a more recent mtime than racy-file.
		sleep 1 &&

		# Update the split index when the shared index contains
		# the racily clean cache entry of racy-file.  A
		# corresponding replacement cache entry with smudged
		# stat data should be added to the new split index, so
		# the file wont appear clean for subsequent git commands.
		git update-index --add other-file &&

		check_cached_diff
	'
done

for trial in 0 1 2 3 4
do
	test_expect_success "add a racily clean file to an already split index #$trial" '
		test_when_finished "rm -f .git/index .git/sharedindex.*" &&

		git update-index --split-index &&

		# The next three commands must be run within the same
		# second.
		echo "cached content" >racy-file &&

		# Update the split index.  The cache entry of racy-file
		# will be stored in the split index.
		git update-index --add racy-file &&

		# File size must stay the same.
		echo "dirty worktree" >racy-file &&

		check_cached_diff
	'
done

for trial in 0 1 2 3 4
do
	test_expect_success "update the split index when it contains a racily clean cache entry #$trial" '
		test_when_finished "rm -f .git/index .git/sharedindex.*" &&

		git update-index --split-index &&

		# The next three commands must be run within the same
		# second.
		echo "cached content" >racy-file &&

		# Update the split index.  The cache entry of racy-file
		# will be stored in the split index.
		git update-index --add racy-file &&

		# File size must stay the same.
		echo "dirty worktree" >racy-file &&

		# Now wait a bit to ensure that the split index written
		# below will get a more recent mtime than racy-file.
		sleep 1 &&

		# Update the split index when it contains the racily
		# clean cache entry of racy-file; the stat data in that
		# entry should be smudged.
		git update-index --add other-file &&

		check_cached_diff
	'
done

test_done
