# Tests for COMPACT interactions with concurrent transactions.
#
# heap_relocate skips tuples it cannot safely move (live conflicting
# updaters, exclusive lockers) but accepts tuples that have only
# key-share lockers, preserving those lockers on the new tuple's xmax.
# These permutations check that:
#
#  - A tuple held under FOR UPDATE in another session is skipped.
#  - A tuple being updated by another session is skipped.
#  - A tuple held under a single FOR KEY SHARE lock is relocated
#    successfully (the locker is preserved).
#  - A tuple held under multiple key-share locks (which form a
#    multixact) is similarly relocated successfully.
#  - A REPEATABLE READ reader sees a stable count across a COMPACT.

setup
{
    CREATE TABLE compact_iso (id int, payload text) WITH (fillfactor = 100);
    INSERT INTO compact_iso
        SELECT g, repeat('z', 200) FROM generate_series(1, 10000) g;
    DELETE FROM compact_iso WHERE id <= 9000;
    -- COMPACT's first internal vacuum pass will prune the dead tuples;
    -- a separate VACUUM here would be ideal but cannot run inside the
    -- isolation tester's setup transaction.
}

teardown
{
    DROP TABLE compact_iso;
}

session "w"
step "w_begin"  { BEGIN; }
step "w_lock"   { SELECT id FROM compact_iso WHERE id = 9500 FOR UPDATE; }
step "w_update" { UPDATE compact_iso SET payload = 'changed' WHERE id = 9500; }
step "w_keyshare" { SELECT id FROM compact_iso WHERE id = 9500 FOR KEY SHARE; }
step "w_commit" { COMMIT; }

# Second key-share locker; combined with w's FOR KEY SHARE this produces
# a multixact on the target tuple, exercising heap_relocate's multixact
# branch.
session "w2"
step "w2_begin"    { BEGIN; }
step "w2_keyshare" { SELECT id FROM compact_iso WHERE id = 9500 FOR KEY SHARE; }
step "w2_commit"   { COMMIT; }

session "c"
step "c_compact" { COMPACT compact_iso; }
step "c_count"   { SELECT count(*) AS rows FROM compact_iso; }
step "c_target"  { SELECT count(*) AS target_rows FROM compact_iso WHERE id = 9500; }

session "r"
step "r_begin"  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "r_count"  { SELECT count(*) AS rows FROM compact_iso; }
step "r_commit" { COMMIT; }


# FOR UPDATE: row is skipped by the relocation pass; COMPACT still
# completes and the row remains intact.
permutation "w_begin" "w_lock" "c_compact" "w_commit" "c_count" "c_target"

# Concurrent UPDATE: row is skipped, count preserved after writer commits.
permutation "w_begin" "w_update" "c_compact" "w_commit" "c_count" "c_target"

# Single FOR KEY SHARE: row is relocated, locker is preserved on the
# new tuple, no row loss after the locker commits.
permutation "w_begin" "w_keyshare" "c_compact" "w_commit" "c_count" "c_target"

# Two FOR KEY SHARE lockers (multixact): row is relocated, both lockers
# are preserved, no row loss after both commit.
permutation "w_begin" "w2_begin" "w_keyshare" "w2_keyshare" "c_compact"
            "w_commit" "w2_commit" "c_count" "c_target"

# REPEATABLE READ reader is unaffected by an intervening COMPACT.
permutation "r_begin" "r_count" "c_compact" "r_count" "r_commit"
