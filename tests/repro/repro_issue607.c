/*
 * repro_issue607.c -- Reproduce-first case for OPEN bug #607.
 *
 * Issue #607: "installing again via install script is dark pattern:
 *              'rebuild index' message followed by delete index action"
 *
 * DESTROYING CODE PATH:
 *   src/cli/cli.c  cbm_cmd_install()  lines 3434-3446
 *   src/cli/cli.c  cbm_remove_indexes()  lines 2440-2468
 *
 *   The sequence is:
 *     1. cbm_cmd_install() at line 3434 calls count_db_indexes() -- finds at
 *        least one .db file in the cache dir.
 *     2. Line 3436 prints:
 *          "Found %d existing index(es) that must be rebuilt:\n"
 *        The word "rebuilt" implies the index data will be preserved and
 *        re-ingested. The user consents on this basis.
 *     3. Line 3444 calls cbm_remove_indexes(home), which in turn calls
 *        cbm_unlink(path) at line 2462 for every .db file it finds.
 *        No rename, no backup, no re-index is triggered. The data is gone.
 *     4. cbm_install_agent_configs() is called (Step 3 of install) -- this
 *        does NOT touch the store and does NOT rebuild the index.
 *     5. The user is left with an empty cache and must re-run
 *        index_repository to recover, with no warning that data was lost.
 *
 * ROOT CAUSE:
 *   The prompt message says "rebuilt" but the action is "delete". There is no
 *   rebuild triggered anywhere in cbm_cmd_install() after the delete. The UI
 *   contract ("rebuilt") is broken by the implementation ("deleted").
 *
 *   See also related bug #557 (mcp.c resolve_store unlink on corrupt detection)
 *   which shares the same pattern of silent, irrecoverable data loss.
 *
 * EXPECTED (correct) behaviour:
 *   After cbm_cmd_install() runs its reinstall path:
 *     (a) the index DB MUST still exist at its original path -- OR --
 *     (b) a rebuild (full re-ingestion) is triggered automatically so the
 *         user ends up with an equivalent index without data loss.
 *   The index MUST NOT be silently deleted with no recovery.
 *
 * ACTUAL (buggy) behaviour:
 *   cbm_remove_indexes() at cli.c:2444 calls cbm_unlink() on every .db
 *   file in the cache dir. After cbm_cmd_install() returns, access(db_path,
 *   F_OK) returns -1 (ENOENT). The index is gone. No rebuild runs.
 *
 * WHY RED on current code:
 *   The final ASSERT_TRUE checks that the index DB still exists after
 *   reinstall. On buggy code cbm_remove_indexes() removes it and no rebuild
 *   creates a new one, so access() returns ENOENT and the assertion fires RED.
 *
 * TRIGGER:
 *   We drive the specific C functions that perform the destroy path, with
 *   CBM_CACHE_DIR redirected to a temp dir so the real user cache is never
 *   touched and the exact db_path is known before the call.
 *
 *   We do NOT call cbm_cmd_install() directly -- that function reads HOME,
 *   prompts the user via prompt_yn(), and copies a real binary. Instead we
 *   call the two functions that embody the dangerous sequence:
 *
 *     cbm_list_indexes()   -- produces the "must be rebuilt" message
 *     cbm_remove_indexes() -- destroys the index (the bug)
 *
 *   This isolates the destroy path without triggering I/O on /proc/self/exe,
 *   editor config detection, or interactive prompts.
 *
 * FIX LOCATION (not implemented here):
 *   src/cli/cli.c  cbm_cmd_install()  around lines 3436/3444:
 *   Either (a) change the prompt to say "delete" not "rebuild" AND trigger
 *   a real rebuild after install, or (b) preserve the DB and trigger an
 *   incremental re-index rather than deleting. The message and the action
 *   must agree; currently they do not.
 */

#include <foundation/compat.h>
#include "test_framework.h"

#include <cli/cli.h>
#include <store/store.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Helper: check whether a file exists ─────────────────────────── */

static int file_exists_607(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* ── Test ─────────────────────────────────────────────────────────────
 *
 * repro_issue607_reinstall_preserves_index
 *
 * Setup (preconditions that must be GREEN to prove the harness is correct):
 *   1. CBM_CACHE_DIR is redirected to a temp dir.
 *   2. A project DB is created at <cache_dir>/cbm-repro607-test.db via
 *      cbm_store_open_path() and closed immediately.
 *   3. The DB file is confirmed to exist (precondition).
 *
 * The dangerous sequence under test:
 *   cbm_list_indexes() -- the "must be rebuilt" message path
 *   cbm_remove_indexes() -- the actual destroy path (the bug)
 *
 * The failing assertion (RED on buggy code):
 *   After cbm_remove_indexes() runs, the DB must still exist at db_path.
 *   On buggy code: cbm_unlink() at cli.c:2462 removes the file.
 *   db_exists == 0, ASSERT_TRUE fires -- RED.
 * ─────────────────────────────────────────────────────────────────── */

#define REPRO607_PROJECT "cbm-repro607-test"

TEST(repro_issue607_reinstall_preserves_index) {
    /* ── Step 1: redirect CBM_CACHE_DIR to a temp dir ─────────────────
     *
     * cbm_resolve_cache_dir() (platform.c:404) checks CBM_CACHE_DIR first.
     * Pointing it at a fresh temp dir ensures:
     *   - the test DB is isolated from the user real cache
     *   - count_db_indexes() / cbm_remove_indexes() see exactly the DB we
     *     create here and nothing else
     */
    char tmp_cache[512];
    snprintf(tmp_cache, sizeof(tmp_cache), "/tmp/cbm_repro607_XXXXXX");
    if (!cbm_mkdtemp(tmp_cache)) {
        ASSERT_NOT_NULL(NULL); /* marks setup failure clearly */
    }

#if defined(_WIN32)
    char ev[600];
    snprintf(ev, sizeof(ev), "CBM_CACHE_DIR=%s", tmp_cache);
    _putenv(ev);
#else
    setenv("CBM_CACHE_DIR", tmp_cache, 1 /* overwrite */);
#endif

    /* ── Step 2: build the DB path we will inspect ────────────────────
     *
     * cbm_remove_indexes() (cli.c:2440) iterates over the cache dir and
     * unlinks every file ending in ".db". Mirror the path formula so
     * db_path matches what cbm_remove_indexes() will delete.
     */
    char db_path[700];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", tmp_cache, REPRO607_PROJECT);

    /* ── Step 3: create a real index DB at db_path ────────────────────
     *
     * cbm_store_open_path() (store.h:199) creates the DB file with full
     * schema (projects table, nodes table, etc.). We insert one project
     * row so the DB is non-empty, then close it. This simulates the state
     * a real user would have after running index_repository once.
     */
    cbm_store_t *setup_store = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(setup_store); /* precondition: store creation must work */

    int upsert_rc = cbm_store_upsert_project(setup_store,
                                              REPRO607_PROJECT,
                                              "/home/user/my-project");
    ASSERT_EQ(upsert_rc, CBM_STORE_OK); /* precondition: row must be written */

    cbm_store_close(setup_store);
    setup_store = NULL;

    /* ── Step 4: verify the DB exists before triggering the danger path */
    ASSERT_TRUE(file_exists_607(db_path)); /* precondition: DB must exist now */

    /* ── Step 5: run the install "rebuild" message path ────────────────
     *
     * cbm_list_indexes() (cli.c:2416) is the function whose output
     * contains "must be rebuilt" (the dark-pattern message). We call it
     * first to mirror the exact sequence inside cbm_cmd_install():
     *
     *   line 3436: printf("Found %d existing index(es) that must be rebuilt:\n", ...)
     *   line 3437: cbm_list_indexes(home);    <-- message visible to user
     *   ...
     *   line 3444: cbm_remove_indexes(home);  <-- silent destroy
     *
     * We pass tmp_cache as a fake home_dir. Because get_cache_dir() calls
     * cbm_resolve_cache_dir() which reads CBM_CACHE_DIR, the home_dir
     * argument is not used in the path -- the env var takes precedence.
     * We pass it anyway to match the real call signature.
     */
    int listed = cbm_list_indexes(tmp_cache /* fake home */);
    ASSERT_GT(listed, 0); /* precondition: must see our DB before removing */

    /* ── Step 6: run the destroying path ──────────────────────────────
     *
     * cbm_remove_indexes() (cli.c:2440) is the function that performs the
     * actual unlink. This is what cbm_cmd_install() calls at line 3444
     * after the user has been told the index will be "rebuilt":
     *
     *   int removed = cbm_remove_indexes(home);  // cli.c:3444
     *
     * cbm_remove_indexes() iterates the cache dir and calls
     * cbm_unlink(path) at cli.c:2462 for every .db file it finds.
     * It returns the count of files successfully unlinked.
     *
     * On buggy code this removes db_path permanently.
     * On correct code the DB would be preserved (or a rebuild triggered).
     */
    (void)cbm_remove_indexes(tmp_cache /* fake home */);

    /* ── Step 7: PRIMARY ASSERTION -- the index DB must still exist ────
     *
     * Correct behaviour: cbm_cmd_install() tells the user the index will be
     * "rebuilt", so EITHER:
     *   (a) the DB was never deleted (preserved in-place), OR
     *   (b) a new DB was created at db_path as a result of the rebuild.
     *
     * In either case, access(db_path, F_OK) must succeed after install.
     *
     * WHY RED on buggy code:
     *   cbm_remove_indexes() calls cbm_unlink(db_path) at cli.c:2462.
     *   No rebuild runs inside cbm_cmd_install() after the delete.
     *   file_exists_607(db_path) returns 0.
     *   ASSERT_TRUE(0) fires -- RED.
     *
     * WHY GREEN on fixed code:
     *   Either cbm_remove_indexes() no longer unlinks the DB (it renames or
     *   preserves it) OR cbm_cmd_install() triggers a rebuild that creates
     *   a fresh DB at db_path before returning.  file_exists_607() returns 1.
     */
    int db_exists = file_exists_607(db_path);

    /* Clean up temp dir (best effort -- before the assertion so the dir
     * is removed even when the assertion fails and longjmp unwinds). */
    unlink(db_path);
    char wal[730], shm[730];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    unlink(wal);
    unlink(shm);
    rmdir(tmp_cache);

#if defined(_WIN32)
    _putenv("CBM_CACHE_DIR=");
#else
    unsetenv("CBM_CACHE_DIR");
#endif

    /*
     * THE KEY ASSERTION -- must be RED on unpatched code:
     *
     * The user was told the index would be REBUILT. After the install code
     * path runs, the index DB must still exist (preserved or rebuilt).
     * On buggy code cbm_remove_indexes() deleted it and no rebuild ran:
     * db_exists == 0 and ASSERT_TRUE fires RED.
     */
    ASSERT_TRUE(db_exists);

    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────────── */
SUITE(repro_issue607) {
    RUN_TEST(repro_issue607_reinstall_preserves_index);
}
