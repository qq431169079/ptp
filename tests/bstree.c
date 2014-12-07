#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../src/utils/cont.h"
#include "../src/utils/bstree_defs.h"

static const unsigned int KEY_MAX_LENGTH = 1024;

/*  Define a bstree type. */
#define BSTREE_NAME my
#define BSTREE_TYPE struct mytype
#define BSTREE_NODE_MEMBER node
#define BSTREE_KEY_TYPE const char *
#define BSTREE_KEY_MEMBER key
BSTREE_TYPE {
    struct bstree_node BSTREE_NODE_MEMBER;
    BSTREE_KEY_TYPE BSTREE_KEY_MEMBER;
};

/* We MUST provide our own key comparison function. See bstree.h. */
static inline int bstree_my_compare(BSTREE_KEY_TYPE keyA, BSTREE_KEY_TYPE keyB)
{
    return strncmp(keyA, keyB, KEY_MAX_LENGTH);
}
#include "../src/utils/bstree.h"


int main ()
{
    /* Btree declaration */
    BSTREE_DECL(tree);
    assert(!tree);
    assert(bstree_is_empty(tree));
    assert(!bstree_my_search(tree, "hello"));

    /* artificial tree */
    struct mytype t1 = { {NULL, {NULL,}}, "eee"};
    tree = &(t1.node);
    assert(bstree_my_search(tree, "eee"));
    struct mytype t2 = { {NULL, {NULL,}}, "aaa"};
    struct mytype t3 = { {NULL, {NULL,}}, "mmm"};
    t1.node.link[LEFT] = &(t2.node);
    t1.node.link[RIGHT] = &(t3.node);
    assert(bstree_my_search(tree, "aaa"));
    assert(bstree_my_search(tree, "mmm"));

    /* start over */
    tree = NULL;
    assert(bstree_is_empty(tree));
    BSTREE_NODE_INIT(t1.node);
    BSTREE_NODE_INIT(t2.node);
    BSTREE_NODE_INIT(t3.node);

    /* Btree insertion */
    assert(bstree_my_insert(&tree, &t1));
    assert(tree == &(t1.node));
    assert(!(*tree).parent);
    assert(bstree_my_search(tree, "eee"));
    assert(!bstree_my_search(tree, "mmm"));
    assert(!bstree_my_insert(&tree, &t1));
    assert(bstree_my_insert(&tree, &t2));
    assert(bstree_my_insert(&tree, &t3));
    assert(t1.node.link[LEFT] == &(t2.node));
    assert(t1.node.link[RIGHT] == &(t3.node));
    assert(bstree_my_search(tree, "mmm"));

    /* Btree deletion */
    struct mytype t4 = { {NULL, {NULL,}}, "rrr"};
    assert(bstree_my_insert(&tree, &t4));
    /*
     *   e
     *  / \
     * a   m
     *      \
     *       r
     */
    bstree_delete(&tree, &t3.node);
    assert(!bstree_my_search(tree, "mmm"));
    assert(t1.node.link[LEFT] == &(t2.node));
    assert(t1.node.link[RIGHT] == &(t4.node));
    assert(bstree_delete(&tree, &t4.node));
    assert(!bstree_my_search(tree, "rrr"));
    assert(t1.node.link[LEFT] == &(t2.node));
    assert(!t1.node.link[RIGHT]);

    BSTREE_DECL(numbers);
    struct mytype one    = { {NULL, {NULL,}}, "1"};
    struct mytype two    = { {NULL, {NULL,}}, "2"};
    struct mytype four   = { {NULL, {NULL,}}, "4"};
    struct mytype five   = { {NULL, {NULL,}}, "5"};
    struct mytype seven  = { {NULL, {NULL,}}, "7"};
    struct mytype eight  = { {NULL, {NULL,}}, "8"};
    struct mytype nine   = { {NULL, {NULL,}}, "9"};
    struct mytype eleven = { {NULL, {NULL,}}, "11"};
    assert(bstree_my_insert(&numbers, &two));
    assert(bstree_my_insert(&numbers, &one));
    assert(bstree_my_insert(&numbers, &five));
    assert(bstree_my_insert(&numbers, &four));
    assert(bstree_my_insert(&numbers, &seven));
    assert(bstree_my_insert(&numbers, &nine));
    assert(bstree_my_insert(&numbers, &eight));
    assert(bstree_my_insert(&numbers, &eleven));
    /*
     *    2
     *   / \
     *  1   5
     *     / \
     *    4   9
     *       / \
     *      7   11
     *       \
     *        8
     */
    assert(bstree_delete(&numbers, &five.node));
    assert(!bstree_my_search(tree, "5"));
    assert(numbers == &two.node);
    assert(nine.node.link[LEFT] == &(eight.node));
    assert(seven.node.link[LEFT] == &(four.node));
    assert(seven.node.link[RIGHT] == &(nine.node));
    assert(seven.node.parent == &(two.node));
    assert(seven.node.parent->link[RIGHT] == &(seven.node));

    assert(bstree_delete(&numbers, &two.node));
    assert(numbers == &four.node);
    assert(four.node.link[LEFT] == &(one.node));
    assert(four.node.link[RIGHT] == &(seven.node));

    /* Bstree navigation/traversal */
    int DIGITS_LEN = 10;
    char *DIGITS_CHAR[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    struct mytype digits_ary[DIGITS_LEN];
    for (int i=0; i<DIGITS_LEN; ++i)
        digits_ary[i] = (struct mytype) { {NULL, {NULL,}}, DIGITS_CHAR[i] };
    int *digits_ins = NULL; // compound literals instead of malloc and memcpy
    BSTREE_DECL(digits);

    int digits_ins_start = 1;
    int digits_ins_len = 9;
    digits_ins = (int[]) {2,1,4,3,8,5,9,6,7};
    for (int i=0; i<digits_ins_len; ++i)
        assert(bstree_my_insert(&digits, &digits_ary[digits_ins[i]]));
    /*
     *    2
     *   / \
     *  1   4
     *     / \
     *    3   8
     *       / \
     *      5   9
     *       \
     *        6
     *         \
     *          7
     */

    /* First/Last */
    assert(bstree_first(digits) == &(digits_ary[1].node));
    assert(!bstree_first(NULL));
    assert(bstree_first(&(digits_ary[8].node)) == &(digits_ary[1].node));
    assert(bstree_first(&(digits_ary[3].node)) == &(digits_ary[1].node));
    assert(bstree_first(&(digits_ary[1].node)) == &(digits_ary[1].node));
    assert(bstree_last(digits) == &(digits_ary[9].node));
    assert(!bstree_last(NULL));
    assert(bstree_last(&(digits_ary[8].node)) == &(digits_ary[9].node));
    assert(bstree_last(&(digits_ary[3].node)) == &(digits_ary[9].node));
    assert(bstree_last(&(digits_ary[9].node)) == &(digits_ary[9].node));

    /* Next */
    for (int i=digits_ins_start; i<(digits_ins_len-1); ++i)
        assert(bstree_next(&(digits_ary[i].node)) == &(digits_ary[i+1].node));

    /* reset */
    digits = NULL;
    for (int i=0; i<DIGITS_LEN; ++i)
        digits_ary[i] = (struct mytype) { {NULL, {NULL,}}, DIGITS_CHAR[i] };

    digits_ins_start = 0;
    digits_ins_len = 10;
    digits_ins = (int[]) {2,1,5,0,4,9,3,7,6,8};
    for (int i=0; i<digits_ins_len; ++i)
        assert(bstree_my_insert(&digits, &digits_ary[digits_ins[i]]));
    /*
     *        2
     *       / \
     *      1   5
     *     /   / \
     *    0   4   9
     *       /   /
     *      3   7
     *         / \
     *        6   8
     */

    /* Next */
    for (int i=digits_ins_start; i<(digits_ins_len-1); ++i)
        assert(bstree_next(&(digits_ary[i].node)) == &(digits_ary[i+1].node));
    assert(!bstree_next(&(digits_ary[9].node)));

    /* Previous */
    for (int i=(digits_ins_len - 1); i>digits_ins_start; --i)
        assert(bstree_prev(&(digits_ary[i].node)) == &(digits_ary[i-1].node));
    assert(!bstree_prev(&(digits_ary[0].node)));

    /* Traversal */
    struct bstree_node *it = bstree_first(digits);
    char *expected = "0123456789"; int i = 0;
    while (it) {
        assert((cont(it, struct mytype, node)->key)[0] == expected[i]);
        it = bstree_next(it); i++;
    }


    return 0;
}
