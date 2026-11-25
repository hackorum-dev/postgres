# Test LISTEN/NOTIFY with notifications sent without commit ordering, that
# is, with pg_notify(channel, payload, false).
#
# Session n1 sends a notification on channel ch_a and is then held in the
# middle of its commit, after its WAL record is flushed but before it is
# marked committed in CLOG, by the "commit-before-clog-update" injection
# point.  Meanwhile session n2 sends a notification on channel ch_b.  The
# listener listens only on ch_b.
#
# Ordered writers serialize on a lock that n1 holds until its commit is
# done, so with ordered notifications on both sides n2 has to wait for n1.
# If either side is unordered, n2 does not wait, and its entry lands in the
# queue after n1's uncommitted one.  The listener is woken by n2 but stops
# reading at n1's entry, so it can't deliver n2's notification until n1 has
# committed.  When n1 commits, it must wake the listener again, rather than
# just moving its queue position past n1's entries (which the listener is
# not interested in), or n2's notification would never be delivered.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE notify_unordered_tab (i int);
}

teardown
{
	DROP TABLE notify_unordered_tab;
	DROP EXTENSION injection_points;
}

session listener
step l_listen	{ LISTEN ch_b; }
step l_check	{ SELECT 1 AS x; }
teardown		{ UNLISTEN *; }

# n1 writes to a table so that its commit has to flush WAL, which is where
# the injection point is.
session n1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('commit-before-clog-update', 'wait');
}
step n1_unordered
{
	INSERT INTO notify_unordered_tab VALUES (1);
	SELECT pg_notify('ch_a', 'from n1', false);
}
step n1_ordered
{
	INSERT INTO notify_unordered_tab VALUES (1);
	NOTIFY ch_a, 'from n1';
}
teardown
{
	SELECT injection_points_detach('commit-before-clog-update');
}

session n2
step n2_unordered	{ SELECT pg_notify('ch_b', 'from n2', false); }
step n2_ordered		{ NOTIFY ch_b, 'from n2'; }

session wakeup
step w_wakeup	{ SELECT injection_points_wakeup('commit-before-clog-update'); }

# Both unordered: n2 doesn't wait for n1.  Nothing can be delivered until n1
# has committed; after that, n2's notification must arrive.
permutation l_listen n1_unordered n2_unordered l_check w_wakeup l_check

# Both ordered: n2 waits for n1, as before.
permutation l_listen n1_ordered n2_ordered w_wakeup l_check

# Unordered n2 doesn't wait for ordered n1.
permutation l_listen n1_ordered n2_unordered l_check w_wakeup l_check

# Ordered n2 doesn't wait for unordered n1, which doesn't take the lock.
permutation l_listen n1_unordered n2_ordered l_check w_wakeup l_check
