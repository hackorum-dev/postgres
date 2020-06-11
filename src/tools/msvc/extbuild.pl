# src/tools/msvc/extbuild.pl

package Mkvcbuild;

use strict;
use warnings;
use Cwd;
use File::Basename;

use Mkvcbuild;

my $solution;
my $bconf     = $ENV{CONFIG}   || "Release";
my $msbflags  = $ENV{MSBFLAGS} || "";
my $makefiledir = $ARGV[0]       || "";
my $pgconfigdir = $ARGV[1]      || "";
my $pgincludedir = "";	# filled by output from pg_config.exe
my $pglibdir = "";	# filled by output from pg_config.exe

sub ProcessPgConfigOutput
{
	my $pgconfigdir = shift;
	my $pgconfig;
	my $output;

	if ($pgconfigdir ne '')
	{
		$pgconfig = $pgconfigdir . "/pg_config.exe";
	}
	else
	{
		$pgconfig = "pg_config.exe";
	}

	if (-e $pgconfig)
	{
		# Run the pg_config command
		$output = `$pgconfig`;

		# parse the stdout of the pg_config command and extract some details.
		if ($output =~ /^INCLUDEDIR\s*=\s*(.*)$/mg)
		{
			$pgincludedir = $1;
		}

		if ($output =~ /^LIBDIR\s*=\s*(.*)$/mg)
		{
			$pglibdir = $1;
		}
	}
	else
	{
		die("Cannot find $pgconfig\n");
	}
}


# Create the vcxproj file for the extension
sub BuildExtensionProjectFile
{
	my $subdir = shift;
	my $scriptdir = shift;
	my $mf     = Project::read_file("$subdir/Makefile");
	my $name;
	my $proj;
	my $visualStudioVersion = VSObjectFactory::DetermineVisualStudioVersion();
	my $solution = CreateSolution($visualStudioVersion);
	if ($mf =~ /^MODULE_big\s*=\s*(.*)$/mg)
	{
		$name = $1;
		$proj = VSObjectFactory::CreateProject($visualStudioVersion, $name, 'dll', $solution);
	}
	elsif ($mf =~ /^MODULES\s*=\s*(.*)$/mg)
	{
		foreach my $mod (split /\s+/, $1)
		{
			$proj = VSObjectFactory::CreateProject($visualStudioVersion, $mod, 'dll', $solution);
			my $filename = $mod . '.c';
			$proj->AddFile("$subdir/$filename");
		}
	}
	elsif ($mf =~ /^PROGRAM\s*=\s*(.*)$/mg)
	{
		$name = $1;
		$proj = VSObjectFactory::CreateProject($visualStudioVersion, $1, 'exe', $solution);
	}
	else
	{
		die("Could not determine extension type for $subdir\n");
	}
	$proj->{abspath} = $scriptdir . "\\..\\..\\..\\";
	$proj->AddDirResourceFile($subdir);
	$proj->AddDir($subdir);
	$proj->AddIncludeDir($pgincludedir);
	$proj->AddIncludeDir("$pgincludedir/server");
	$proj->AddIncludeDir("$pgincludedir/server/port/win32");
	$proj->AddIncludeDir("$pgincludedir/server/port/win32_msvc");
	$proj->AddLibrary($pglibdir . '/postgres.lib');
	$proj->AddLibrary($pglibdir . '/libpq.lib');

	#$proj->AddReference("$pglibdir/postgres");
	# Are there any output data files to build?
	GenerateContribSqlFiles($subdir, $mf);
	$proj->Save();
	return $name;
}


my $num_args = $#ARGV + 1;

if ($num_args < 1)
{
	print("Usage: extbuild.pl <path_to_make_file> [path_to_pg_config] [DEBUG]");
	exit;
}

# buildenv.pl is for specifying the build environment settings
# it should contain lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if (-e "src/tools/msvc/buildenv.pl")
{
	do "./src/tools/msvc/buildenv.pl";
}
elsif (-e "./buildenv.pl")
{
	do "./buildenv.pl";
}

if ($num_args >= 3)
{
	if (uc($ARGV[2]) eq 'DEBUG')
	{
		$bconf = "Debug";
	}
	elsif (uc($ARGV[2]) eq 'RELEASE')
	{
		$bconf = "Release";
	}
}

my $name;

my $scriptdir = dirname(Cwd::abs_path(__FILE__));
chdir $makefiledir;

ProcessPgConfigOutput($pgconfigdir);

print("Makefile dir         = $makefiledir\n");
print("Postgres include dir = $pgincludedir\n");
print("Building             = $bconf\n");

$name = BuildExtensionProjectFile($makefiledir, $scriptdir);

system(
	"msbuild $makefiledir/$name.vcxproj /verbosity:normal $msbflags /p:Configuration=$bconf"
);




