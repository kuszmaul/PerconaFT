/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

// Verify that a message with an old msn is ignored
// by toku_apply_msg_to_leaf()
//
// method:
//  - inject valid message, verify that new value is in row
//  - inject message with same msn and new value, verify that original value is still in key  (verify msg.msn == node.max_msn is rejected)
//  - inject valid message with new value2, verify that row has new value2 
//  - inject message with old msn, verify that row still has value2   (verify msg.msn < node.max_msn is rejected)


// TODO: 
//  - verify that no work is done by messages that should be ignored (via workdone arg to ft_leaf_put_msg())
//  - maybe get counter of messages ignored for old msn (once the counter is implemented in ft-ops.c)

#include "ft-internal.h"
#include <ft-cachetable-wrappers.h>

#include "test.h"

static FTNODE
make_node(FT_HANDLE ft, int height) {
    FTNODE node = NULL;
    int n_children = (height == 0) ? 1 : 0;
    toku_create_new_ftnode(ft, &node, height, n_children);
    if (n_children) BP_STATE(node,0) = PT_AVAIL;
    return node;
}

static void
append_leaf(FT_HANDLE ft, FTNODE leafnode, void *key, uint32_t keylen, void *val, uint32_t vallen) {
    assert(leafnode->height == 0);

    DBT thekey; toku_fill_dbt(&thekey, key, keylen);
    DBT theval; toku_fill_dbt(&theval, val, vallen);
    DBT badval; toku_fill_dbt(&badval, (char*)val+1, vallen);
    DBT val2;   toku_fill_dbt(&val2, (char*)val+2, vallen);

    struct check_pair pair  = {keylen, key, vallen, val, 0};
    struct check_pair pair2 = {keylen, key, vallen, (char*)val+2, 0};

    // apply an insert to the leaf node
    MSN msn = next_dummymsn();
    ft->ft->h->max_msn_in_ft = msn;
    ft_msg msg(&thekey, &theval, FT_INSERT, msn, toku_xids_get_root_xids());
    txn_gc_info gc_info(nullptr, TXNID_NONE, TXNID_NONE, false);

    toku_ft_leaf_apply_msg(
        ft->ft->cmp,
        ft->ft->update_fun,
        leafnode,
        -1,
        msg,
        &gc_info,
        nullptr,
        nullptr,
        nullptr);
    {
        int r = toku_ft_lookup(ft, &thekey, lookup_checkf, &pair);
        assert(r==0);
        assert(pair.call_count==1);
    }

    ft_msg badmsg(&thekey, &badval, FT_INSERT, msn, toku_xids_get_root_xids());
    toku_ft_leaf_apply_msg(
        ft->ft->cmp,
        ft->ft->update_fun,
        leafnode,
        -1,
        badmsg,
        &gc_info,
        nullptr,
        nullptr,
        nullptr);

    // message should be rejected for duplicate msn, row should still have original val
    {
	      int r = toku_ft_lookup(ft, &thekey, lookup_checkf, &pair);
	      assert(r==0);
	      assert(pair.call_count==2);
    }

    // now verify that message with proper msn gets through
    msn = next_dummymsn();
    ft->ft->h->max_msn_in_ft = msn;
    ft_msg msg2(&thekey, &val2,  FT_INSERT, msn, toku_xids_get_root_xids());
    toku_ft_leaf_apply_msg(
        ft->ft->cmp,
        ft->ft->update_fun,
        leafnode,
        -1,
        msg2,
        &gc_info,
        nullptr,
        nullptr,
        nullptr);

    // message should be accepted, val should have new value
    {
	      int r = toku_ft_lookup(ft, &thekey, lookup_checkf, &pair2);
	      assert(r==0);
	      assert(pair2.call_count==1);
    }

    // now verify that message with lesser (older) msn is rejected
    msn.msn = msn.msn - 10;
    ft_msg msg3(&thekey, &badval, FT_INSERT, msn, toku_xids_get_root_xids());
    toku_ft_leaf_apply_msg(
        ft->ft->cmp,
        ft->ft->update_fun,
        leafnode,
        -1,
        msg3,
        &gc_info,
        nullptr,
        nullptr,
        nullptr);

    // message should be rejected, val should still have value in pair2
    {
	      int r = toku_ft_lookup(ft, &thekey, lookup_checkf, &pair2);
	      assert(r==0);
	      assert(pair2.call_count==2);
    }

    // dont forget to dirty the node
    leafnode->dirty = 1;
}

static void 
populate_leaf(FT_HANDLE ft, FTNODE leafnode, int k, int v) {
    char vbuf[32]; // store v in a buffer large enough to dereference unaligned int's
    memset(vbuf, 0, sizeof vbuf);
    memcpy(vbuf, &v, sizeof v);
    append_leaf(ft, leafnode, &k, sizeof k, vbuf, sizeof v);
}

static void 
test_msnfilter(int do_verify) {
    int r;

    // cleanup
    const char *fname = TOKU_TEST_FILENAME;
    r = unlink(fname);
    if (r != 0) {
        assert(r == -1);
        assert(get_error_errno() == ENOENT);
    }

    // create a cachetable
    CACHETABLE ct = NULL;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    // create the ft
    TOKUTXN null_txn = NULL;
    FT_HANDLE ft = NULL;
    r = toku_open_ft_handle(fname, 1, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r == 0);

    FTNODE newroot = make_node(ft, 0);

    // set the new root to point to the new tree
    toku_ft_set_new_root_blocknum(ft->ft, newroot->blocknum);

    // KLUDGE: Unpin the new root so toku_ft_lookup() can pin it.  (Pin lock is no longer a recursive
    //         mutex.)  Just leaving it unpinned for this test program works  because it is the only 
    //         node in the cachetable and won't be evicted.  The right solution would be to lock the 
    //         node and unlock it again before and after each message injection, but that requires more
    //         work than it's worth (setting up dummy callbacks, etc.)
    //         
    toku_unpin_ftnode(ft->ft, newroot);

    populate_leaf(ft, newroot, htonl(2), 1);

    if (do_verify) {
        r = toku_verify_ft(ft);
        assert(r == 0);
    }

    // flush to the file system
    r = toku_close_ft_handle_nolsn(ft, 0);     
    assert(r == 0);

    // shutdown the cachetable
    toku_cachetable_close(&ct);
}

static int
usage(void) {
    return 1;
}

int
test_main (int argc , const char *argv[]) {
    int do_verify = 1;
    initialize_dummymsn();
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(arg, "-q") == 0) {
            verbose = 0;
            continue;
        }
        if (strcmp(arg, "--verify") == 0 && i+1 < argc) {
            do_verify = atoi(argv[++i]);
            continue;
        }
        return usage();
    }
    test_msnfilter(do_verify);
    return 0;
}
