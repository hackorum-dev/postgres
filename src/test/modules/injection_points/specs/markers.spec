setup
{
	CREATE EXTENSION injection_points;
}
teardown
{
	DROP EXTENSION injection_points;
}

# Wait happens in the first session, wakeup in the second session.
session s1
setup	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('injection-points-wait-1', 'wait');
}
step after	{ SELECT injection_points_run('injection-points-wait-1'); }

session s2
setup	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('injection-points-wait-2', 'wait');
}
step before	{ SELECT injection_points_run('injection-points-wait-2'); }

session s3
step wakeup1	{ SELECT injection_points_wakeup('injection-points-wait-1'); }
step detach1	{ SELECT injection_points_detach('injection-points-wait-1'); }
step wakeup2	{ SELECT injection_points_wakeup('injection-points-wait-2'); }
step detach2	{ SELECT injection_points_detach('injection-points-wait-2'); }

permutation
	after(before)
	before
	detach1
	wakeup1
	detach2
	wakeup2

permutation
	after(before)
	wakeup1
	before
	detach1
	detach2
	wakeup2