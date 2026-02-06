# Test local injection points with custom injection function.
#

setup
{
	CREATE EXTENSION injection_points;
}
teardown
{
	DROP EXTENSION injection_points;
}

# Session 1 attaches a local injection point
session s1
setup	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('TestInjectionNoticeFunc', 'injection_points',
	  'injection_notice', 'attach argument');
}
step run1	{
	SELECT injection_points_run('TestInjectionNoticeFunc', 'run argument');
}
step list1	{
	SELECT point_name, library, function FROM injection_points_list()
	  ORDER BY point_name COLLATE "C";
}
step detach1	{
	SELECT injection_points_detach('TestInjectionNoticeFunc');
}

# Session 2 tries to run and list the injection point. Since the injection point
# is local to session 1, it should not be run in this session.
session s2
step run2	{
	SELECT injection_points_run('TestInjectionNoticeFunc', 'run argument from other backend');
}
step list2	{
	SELECT point_name, library, function FROM injection_points_list()
	  ORDER BY point_name COLLATE "C";
}
step detach2	{
	SELECT injection_points_detach('TestInjectionNoticeFunc');
}

# Attach in s1, verify s1 can run it and see it in list
# Then verify s2 can see it in list but running it does nothing
# Detaching from s2 will succeed which will result in detach from s1 to fail. Reversing the detach sequence, detach from s1 will succeed causing subsequent detach from s2 to fail.
permutation list1 run1 list2 run2 detach2 detach1
permutation list1 run1 list2 run2 detach1 detach2
