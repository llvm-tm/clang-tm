#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// ── Persistent RBTree (index-based, ASLR-safe) ─────────────────────
// Uses global TM variables so DudeTM's symbol persistence saves/restores
// the entire tree state across process restarts.
// Integer indices replace raw pointers — valid across any base address.

#define MAX_NODES 256

enum Color : int32_t { RED, BLACK };

struct TreeNode {
    int32_t key;
    int32_t val;
    Color   color;
    int32_t left;    // index, -1 = null
    int32_t right;
    int32_t parent;
};

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))

TM TreeNode g_nodes[MAX_NODES];
TM int32_t  g_root   = -1;
TM int32_t  g_count  = 0;
TM int32_t  g_next   = 0;

static int32_t alloc_node() {
    if (g_next >= MAX_NODES) return -1;
    int32_t idx = g_next++;
    g_nodes[idx].key    = 0;
    g_nodes[idx].val    = 0;
    g_nodes[idx].color  = RED;
    g_nodes[idx].left   = -1;
    g_nodes[idx].right  = -1;
    g_nodes[idx].parent = -1;
    return idx;
}

TX void tx_insert(int32_t key, int32_t val) {
    int32_t z = alloc_node();
    if (z < 0) return;
    g_nodes[z].key = key;
    g_nodes[z].val = val;
    g_count++;

    // BST insert
    int32_t y = -1;
    int32_t x = g_root;
    while (x >= 0) {
        y = x;
        if (key < g_nodes[x].key)
            x = g_nodes[x].left;
        else
            x = g_nodes[x].right;
    }
    g_nodes[z].parent = y;
    if (y < 0) {
        g_root = z;
    } else if (key < g_nodes[y].key) {
        g_nodes[y].left = z;
    } else {
        g_nodes[y].right = z;
    }

    // Fixup (red-black rebalance)
    while (g_nodes[z].parent >= 0 &&
           g_nodes[g_nodes[z].parent].color == RED) {
        int32_t p = g_nodes[z].parent;
        int32_t gp = g_nodes[p].parent;
        if (gp < 0) break;
        if (p == g_nodes[gp].left) {
            int32_t u = g_nodes[gp].right;
            if (u >= 0 && g_nodes[u].color == RED) {
                g_nodes[p].color  = BLACK;
                g_nodes[u].color  = BLACK;
                g_nodes[gp].color = RED;
                z = gp;
                continue;
            }
            if (z == g_nodes[p].right) {
                // left rotate at p
                int32_t t = g_nodes[z].left;
                g_nodes[z].left = p;
                g_nodes[p].parent = z;
                g_nodes[p].right = t;
                if (t >= 0) g_nodes[t].parent = p;
                g_nodes[z].parent = gp;
                if (gp >= 0) {
                    if (g_nodes[gp].left == p) g_nodes[gp].left = z;
                    else g_nodes[gp].right = z;
                } else { g_root = z; }
                p = z;
                z = p;  // re-fetch after rotation
            }
            // right rotate at gp
            int32_t gpp_1 = g_nodes[gp].parent;
            int32_t b = g_nodes[p].right;
            g_nodes[gp].left = b;
            if (b >= 0) g_nodes[b].parent = gp;
            g_nodes[p].right = gp;
            g_nodes[gp].parent = p;
            g_nodes[p].parent = gpp_1;
            if (gpp_1 >= 0) {
                if (g_nodes[gpp_1].left == gp)
                    g_nodes[gpp_1].left = p;
                else
                    g_nodes[gpp_1].right = p;
            } else { g_root = p; }
            g_nodes[p].color = BLACK;
            g_nodes[gp].color = RED;
            break;
        } else {
            // Mirror case
            int32_t u = g_nodes[gp].left;
            if (u >= 0 && g_nodes[u].color == RED) {
                g_nodes[p].color  = BLACK;
                g_nodes[u].color  = BLACK;
                g_nodes[gp].color = RED;
                z = gp;
                continue;
            }
            if (z == g_nodes[p].left) {
                // right rotate at p
                int32_t t = g_nodes[z].right;
                g_nodes[z].right = p;
                g_nodes[p].parent = z;
                g_nodes[p].left = t;
                if (t >= 0) g_nodes[t].parent = p;
                g_nodes[z].parent = gp;
                if (gp >= 0) {
                    if (g_nodes[gp].right == p) g_nodes[gp].right = z;
                    else g_nodes[gp].left = z;
                } else { g_root = z; }
                p = z;
                z = p;
            }
            // left rotate at gp
            int32_t gpp_2 = g_nodes[gp].parent;
            int32_t b = g_nodes[p].left;
            g_nodes[gp].right = b;
            if (b >= 0) g_nodes[b].parent = gp;
            g_nodes[p].left = gp;
            g_nodes[gp].parent = p;
            g_nodes[p].parent = gpp_2;
            if (gpp_2 >= 0) {
                if (g_nodes[gpp_2].right == gp)
                    g_nodes[gpp_2].right = p;
                else
                    g_nodes[gpp_2].left = p;
            } else { g_root = p; }
            g_nodes[p].color = BLACK;
            g_nodes[gp].color = RED;
            break;
        }
    }
    if (g_root >= 0) g_nodes[g_root].color = BLACK;
}

TX int32_t tx_lookup(int32_t key) {
    int32_t x = g_root;
    while (x >= 0) {
        if (key == g_nodes[x].key) return g_nodes[x].val;
        if (key < g_nodes[x].key) x = g_nodes[x].left;
        else x = g_nodes[x].right;
    }
    return -1;  // not found
}

TX int32_t tx_count() {
    return g_count;
}

static int32_t verify_inorder(int32_t idx, int32_t* keys, int32_t* vals, int32_t pos) {
    if (idx < 0) return pos;
    pos = verify_inorder(g_nodes[idx].left, keys, vals, pos);
    keys[pos] = g_nodes[idx].key;
    vals[pos] = g_nodes[idx].val;
    pos++;
    pos = verify_inorder(g_nodes[idx].right, keys, vals, pos);
    return pos;
}

int main(int argc, char** argv) {
    const char* mode = "run";
    int count = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode = argv[++i];
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
    }

    if (strcmp(mode, "init") == 0) {
        printf("MODE INIT: root=%d count=%d next=%d\n",
               (int)g_root, (int)g_count, (int)g_next);

    } else if (strcmp(mode, "run") == 0) {
        printf("MODE RUN: count=%d initial_root=%d initial_count=%d\n",
               count, (int)g_root, (int)g_count);

        for (int i = 0; i < count; i++) {
            int key = i * 10;
            int val = i * 100;
            tx_insert(key, val);
        }

        printf("  after insert: root=%d count=%d next=%d\n",
               (int)g_root, (int)g_count, (int)g_next);

    } else     if (strcmp(mode, "verify") == 0) {
        int expected = (argc > 3) ? atoi(argv[argc - 1]) : 0;

        printf("MODE VERIFY: root=%d count=%d expected=%d\n",
               (int)g_root, (int)g_count, expected);

        bool ok = true;
        if ((int)g_count != expected) {
            printf("  FAIL: count=%d expected=%d\n", (int)g_count, expected);
            ok = false;
        }

        // Read keys/values via TX
        int keys[MAX_NODES], vals[MAX_NODES];
        int n = verify_inorder(g_root, keys, vals, 0);

        printf("  inorder (%d nodes):", n);
        for (int i = 0; i < n; i++)
            printf(" (%d,%d)", keys[i], vals[i]);
        printf("\n");

        for (int i = 0; i < n; i++) {
            int expected_key = i * 10;
            int expected_val = i * 100;
            if (keys[i] != expected_key || vals[i] != expected_val) {
                printf("  FAIL at pos %d: got (%d,%d) expected (%d,%d)\n",
                       i, keys[i], vals[i], expected_key, expected_val);
                ok = false;
            }
        }

        printf("  %s\n", ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }

    return 0;
}
