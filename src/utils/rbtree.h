#ifndef RBTREE_H
#define RBTREE_H

/**
 * A red-black binary tree.
 *
 * Better than hash if order matters.
 * Inspired by the Linux kernel and Julienne Walker.
 *
 * Consumer MUST provide a key comparison function:
 *
 *   int name##_compare(RBTREE_KEY_TYPE keyA, RBTREE_KEY_TYPE keyB);
 */
#include "bstree.h"

/**
 * Red-black tree rules:
 * 1) Every node has a color either red or black.
 * 2) Root of tree is always black.
 * 3) There are no two adjacent red nodes (A red node cannot have a red parent
 *    or red child).
 * 4) Every path from root to a NULL node has same number of black nodes.
 */
struct rbtree_node {
    int color;
#define RB_RED   0
#define RB_BLACK 1
    BSTREE_MEMBERS(rbtree_node)
};

/**
 * Declare a rbtree.
 */
#define RBTREE_DECL(tree) BSTREE_GENERIC_DECL(rbtree_node, tree)

/**
 * Init a to-be-inserted btree node.
 *
 * The root node has parent NULL.
 */
#define RBTREE_NODE_INIT(node) do {             \
        (node).color = RB_RED;                  \
        BSTREE_NODE_INIT(node);                 \
    } while (/*CONSTCOND*/ 0)


#define BSTREE_KEY_TYPE RBTREE_KEY_TYPE
#define RBTREE_GENERATE(name, type, field, key, opt)    \
    BSTREE_GENERATE_DELETE(name, type, opt)             \
    BSTREE_GENERATE_INSERT(name, type, field, key, opt) \
    BSTREE_GENERATE_SEARCH(name, type, field, key)      \
    RBTREE_GENERATE_INSERT(name, field, opt)

BSTREE_GENERATE_BASE(rbtree)

/**
 * Rotate at @root in direction @dir.
 *
 *     x             y
 *    / \           / \
 *   a   y   <->   x   c
 *      / \       / \
 *     b   c     a   b
 */
void rbtree_rotate(struct rbtree_node *root, int dir,
                   struct rbtree_node **parent_link)
{
    struct rbtree_node *new = root->link[!dir];

    rbtree_link_node(new->link[dir], root, &(root->link[!dir]));
    rbtree_link_node(new, root->parent, parent_link);
    rbtree_link_node(root, new, &(new->link[dir]));
}

/**
 * Double rotation
 *
 * Rotate in opposite direction at child of @root, then in the @dir direction
 * at @root.
 *
 *       z*               z             y
 *      / \              / \          /   \
 *     x   d            y   d        x     z
 *    / \       ->     / \     ->   / \   / \
 *   a   y            x   c        a   b c   d
 *      / \          / \
 *     b   c        a   b
 */
void rbtree_rotate_double(struct rbtree_node *root, int dir,
                          struct rbtree_node **parent_link)
{
    rbtree_rotate(root->link[!dir], !dir, &(root->link[!dir]));
    rbtree_rotate(root, dir, parent_link);
}

#define RBTREE_GENERATE_INSERT(name, field, opt)                        \
bool name##_insert(struct rbtree_node **tree, struct name *data)        \
{                                                                       \
    if (!name##opt##_insert(tree, data))                                \
        return false;                                                   \
                                                                        \
    struct rbtree_node *node = &data->field;                            \
    while (node->parent) {                                              \
        node->color = RB_RED;                                           \
                                                                        \
        /* Parent black, nothing to do. */                              \
        if (node->parent->color == RB_BLACK)                            \
            return true;                                                \
                                                                        \
        /* Red parent => has a grand-parent. */                         \
        struct rbtree_node *parent = node->parent;                      \
        int par_dir = RIGHT_IF(parent == parent->parent->link[RIGHT]);  \
        struct rbtree_node *uncle = parent->parent->link[!par_dir];     \
                                                                        \
        /*
         * Red uncle => reverse relatives' color.
         *
         *    B        r
         *   / \      / \
         *  r   r -> B   B
         *   \        \
         *    r        r
         */                                                             \
        if (uncle && uncle->color == RB_RED) {                          \
            parent->color = RB_BLACK;                                   \
            uncle->color = RB_BLACK;                                    \
            parent->parent->color = RB_RED;                             \
            node = parent->parent;                                      \
        }                                                               \
        /*
         * Black or no uncle => rotate and recolor.
         */                                                             \
        else {                                                          \
            int dir = RIGHT_IF(node == parent->link[RIGHT]);            \
            struct rbtree_node **top =                                  \
                rbtree_parent_link(tree, parent->parent);               \
            /*
             *       B         (r)B
             *      / \        /   \
             *     r   B  ->  r*   (B)r
             *    / \              / \
             *   r*  B            B   B
             */                                                         \
            if (par_dir == dir)                                         \
                rbtree_rotate(parent->parent, !par_dir, top);           \
            /*
             *      B         (r*)B
             *     / \        /   \
             *    r   B  ->  r    (B)r
             *     \                \
             *      r*               B
             */                                                         \
            else                                                        \
                rbtree_rotate_double(parent->parent, !par_dir, top);    \
            (*top)->color = RB_BLACK;                                   \
            (*top)->link[!par_dir]->color = RB_RED;                     \
        }                                                               \
    }                                                                   \
                                                                        \
    node->color = RB_BLACK;                                             \
    return true;                                                        \
}

#endif /* RBTREE_H */
