
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>  /* 平台配置头：编译期特性探测与基础类型 */
#include <ngx_core.h>  /* 核心头：ngx_rbtree_t/ngx_inline 等定义 */


/*
 * The code is based on the algorithm described in the "Introduction
 * to Algorithms" by Cormen, Leiserson and Rivest.
 */

/* 置红：color 直接用 1/0 整数而非枚举，节点紧凑、缓存友好 */
#define ngx_rbt_red(node)           ((node)->color = 1)
/* 置黑 */
#define ngx_rbt_black(node)         ((node)->color = 0)
/* 判红：非零即红 */
#define ngx_rbt_is_red(node)        ((node)->color)
/* 判黑：非红即黑(哨兵 NIL 视为黑) */
#define ngx_rbt_is_black(node)      (!ngx_rbt_is_red(node))
/* 复制颜色：删除时替换节点继承被删颜色，使该位置黑高保持不变 */
#define ngx_rbt_copy_color(n1, n2)  (n1->color = n2->color)


/* 左旋声明：node 下沉为右孩子的左孩子，右孩子顶替其位；不破坏 BST 有序性，仅改指针、O(1) */
ngx_inline void ngx_rbtree_left_rotate(ngx_rbtree_t **root,
                                       ngx_rbtree_t *sentinel,  /* NIL 哨兵节点 */
                                       ngx_rbtree_t *node);  /* 旋转支点 */
/* 右旋声明：左旋的镜像操作 */
ngx_inline void ngx_rbtree_right_rotate(ngx_rbtree_t **root,
                                        ngx_rbtree_t *sentinel,  /* NIL 哨兵节点 */
                                        ngx_rbtree_t *node);  /* 旋转支点 */


/*
 * ngx_rbtree_insert()：红黑树插入(非递归实现)。
 * 先按 BST 规则下行挂上节点，再将其染红并自底向上修复性质：新红节点可能
 * 与红父冲突(性质4)，按"叔叔颜色"分情形处理——至多 O(log n) 次变色、
 * 至多 2 次旋转。定时器模块即以此树按毫秒到期时刻为键组织事件。
 */
void ngx_rbtree_insert(ngx_rbtree_t **root, ngx_rbtree_t *sentinel,  /* 插入节点：BST 下行 + 红黑修复 */
                       ngx_rbtree_t *node)  /* 待插入节点(键已由调用者填好) */
{  /* 函数体开始 */
    ngx_rbtree_t  *temp;  /* 下行游标/父节点/叔叔节点，随阶段复用 */

    /* a binary tree insert */

    if (*root == sentinel) {  /* 根指针等于哨兵即空树：新节点直接成为根 */
        node->parent = NULL;  /* 根没有父节点 */
        node->left = sentinel;  /* 左孩子指向哨兵(代替 NULL 叶子) */
        node->right = sentinel;  /* 右孩子指向哨兵 */
        ngx_rbt_black(node);  /* 根必须为黑(性质2)：首节点直接染黑，无需修复 */
        *root = node;  /* 更新树根 */

        return;  /* 空树情形完成，直接返回 */
    }  /* 结束空树分支 */

    temp = *root;  /* 从根开始按 BST 规则下行 */

    for ( ;; ) {  /* 非递归下行：不耗函数调用栈，树再深也不怕栈溢出 */
        if (node->key < temp->key) {  /* 键小走左：定时器场景即"到期更早"的排左边 */
            if (temp->left == sentinel) {  /* 左侧已到空位：挂为左孩子 */
                temp->left = node;  /* 完成链接 */
                break;  /* 插入位置确定，跳出下行循环 */
            }  /* 结束左空判断 */

            temp = temp->left;  /* 继续向左子树探 */
            continue;  /* 进入下一轮比较 */
        }  /* 结束"走左"分支 */

        if (temp->right == sentinel) {  /* 键≥当前节点：右空则挂右(相等键约定归右，次序稳定) */
            temp->right = node;  /* 完成链接 */
            break;  /* 跳出循环 */
        }  /* 结束右空判断 */

        temp = temp->right;  /* 向右子树继续下探 */
        continue;  /* 进入下一轮比较 */
    }  /* BST 下行循环结束 */

    node->parent = temp;  /* temp 即插入位置的父节点，回填父指针 */
    node->left = sentinel;  /* 新节点的孩子均置为哨兵(叶子) */
    node->right = sentinel;  /* 右孩子置为哨兵 */


    /* re-balance tree */

    ngx_rbt_red(node);  /* 新节点染红：红不改变任何路径黑高(性质5)，把破坏限制在性质4(红-红) */

    while (node != *root && ngx_rbt_is_red(node->parent)) {  /* 出现"红父"才违反性质4，需向根方向逐层修复 */

        if (node->parent == node->parent->parent->left) {  /* 父是祖父的左孩子(右孩子情形为其镜像) */
            temp = node->parent->parent->right;  /* temp 取"叔叔"节点：修复方案由其颜色决定 */

            if (ngx_rbt_is_red(temp)) {  /* 情形1：叔叔为红——只变色不旋转，把冲突上推一层 */
                ngx_rbt_black(node->parent);  /* 父染黑 */
                ngx_rbt_black(temp);  /* 叔染黑：两支子树黑高各 +1，仍保持相等 */
                ngx_rbt_red(node->parent->parent);  /* 祖父染红：补回子树黑高，维持性质5 */
                node = node->parent->parent;  /* 冲突上移到祖父：若其父也是红则继续循环 */
            } else {  /* 情形2/3：叔叔为黑(或哨兵)——必须旋转 */
                if (node == node->parent->right) {  /* 情形2：node 是"内侧"孩子，先转成直线型(情形3) */
                    node = node->parent;  /* 旋转后原父变为孩子，修复焦点随之切换 */
                    ngx_rbtree_left_rotate(root, sentinel, node);  /* 左旋父节点：折线转直线 */
                }  /* 结束情形2预处理 */

                ngx_rbt_black(node->parent);  /* 情形3：父染黑(将成为子树新根) */
                ngx_rbt_red(node->parent->parent);  /* 祖父染红：接住原子树的黑高 */
                /* 右旋祖父：黑父升级、红祖降级，黑高不变且红-红冲突消除 */
                ngx_rbtree_right_rotate(root, sentinel, node->parent->parent);
            }  /* 结束叔叔为黑分支 */

        } else {  /* 镜像情形：父节点是祖父的右孩子 */
            temp = node->parent->parent->left;  /* 叔叔在左侧 */

            if (ngx_rbt_is_red(temp)) {  /* 镜像情形1：叔叔红，变色并上推 */
                ngx_rbt_black(node->parent);  /* 父染黑 */
                ngx_rbt_black(temp);  /* 叔染黑 */
                ngx_rbt_red(node->parent->parent);  /* 祖父染红 */
                node = node->parent->parent;  /* 冲突继续上移 */
            } else {  /* 镜像情形2/3：叔叔黑，需旋转 */
                if (node == node->parent->left) {  /* 镜像情形2：内侧孩子，先右旋成直线型 */
                    node = node->parent;  /* 焦点切到原父 */
                    ngx_rbtree_right_rotate(root, sentinel, node);  /* 右旋消去折线 */
                }  /* 结束镜像情形2 */

                ngx_rbt_black(node->parent);  /* 镜像情形3：父染黑 */
                ngx_rbt_red(node->parent->parent);  /* 祖父染红 */
                /* 左旋祖父：镜像的终结旋转，修复完成 */
                ngx_rbtree_left_rotate(root, sentinel, node->parent->parent);
            }  /* 结束镜像叔叔为黑分支 */
        }  /* 结束左/右对称两大分支 */

    }  /* 插入修复循环结束 */

    ngx_rbt_black(*root);  /* 收尾强制根黑(性质2)：无论红色被推到多高，染一次即收束 */
}  /* ngx_rbtree_insert 结束 */


/*
 * ngx_rbtree_delete()：红黑树删除(非递归实现)。
 * 先按 BST 删除：至多一个孩子的节点直接摘除；两孩子俱全的用后继(右子树
 * 最小节点 subst)顶替。摘走黑节点会破坏性质5(黑高相等)，需按"兄弟 w 的
 * 颜色与其孩子颜色"四种情形做删除修复；subst 为红则无需修复直接返回。
 */
void ngx_rbtree_delete(ngx_rbtree_t **root, ngx_rbtree_t *sentinel,  /* 删除节点：BST 删除 + 黑高修复 */
                       ngx_rbtree_t *node)  /* 待删除节点 */
{  /* 函数体开始 */
    ngx_int_t      is_red;  /* 记录替换节点的颜色：红可直接删，黑必须修复 */
    ngx_rbtree_t  *subst, *temp, *w;  /* subst 替换者/temp 衔接子树/w 兄弟节点 */

    /* a binary tree delete */

    if (node->left == sentinel) {  /* 情形A：左子树为空——右孩子(或哨兵)直接顶替 */
        temp = node->right;  /* temp：接替 node 位置的子树 */
        subst = node;  /* 真正被摘除的就是 node 本身 */

    } else if (node->right == sentinel) {  /* 情形B：右子树为空——左孩子顶替 */
        temp = node->left;  /* 左子树整体上移 */
        subst = node;  /* 摘除 node 本身 */

    } else {  /* 情形C：两子树俱全——用后继顶替 node 的位置 */
        subst = ngx_rbtree_min(node->right, sentinel);  /* 后继：键恰大于 node，且至多无左孩子便于摘除 */

        if (subst->left != sentinel) {  /* 防御分支：后继理论上无左孩子 */
            temp = subst->left;  /* 衔接子树取左 */
        } else {  /* 常规路径 */
            temp = subst->right;  /* 后继的右子树(可能为哨兵)上移 */
        }  /* 结束防御判断 */
    }  /* 结束三种 BST 删除情形 */

    if (subst == *root) {  /* 删除的是根：孩子(或哨兵)直接升为新根 */
        *root = temp;  /* 更新根指针 */
        ngx_rbt_black(temp);  /* 根必须黑(性质2)；哨兵本就是黑的 */

        /* DEBUG stuff */
        node->left = NULL;  /* 调试用：清空已删节点的树指针 */
        node->right = NULL;  /* 防止悬挂引用 */
        node->parent = NULL;  /* 清空父指针 */
        node->key = 0;  /* 键清零，便于发现误用 */

        return;  /* 根被替换后无黑高失衡问题，无需修复 */
    }  /* 结束删根分支 */

    is_red = ngx_rbt_is_red(subst);  /* 先记下替换者颜色：随后链接被搬动，颜色信息要在丢失前留存 */

    if (subst == subst->parent->left) {  /* 把 temp 接到 subst 腾出的位置 */
        subst->parent->left = temp;  /* temp 顶替为左孩子 */

    } else {  /* subst 原是右孩子 */
        subst->parent->right = temp;  /* temp 顶替为右孩子 */
    }  /* 结束 temp 接链 */

    if (subst == node) {  /* 情形A/B：摘的就是 node 本身，链接无需搬移 */

        temp->parent = subst->parent;  /* temp(node 的孩子)直接继承 node 的父指针 */

    } else {  /* 情形C：要把 subst 搬到 node 的位置上 */

        if (subst->parent == node) {  /* 后继恰是 node 的右孩子：temp 的父应指向 subst 自身 */
            temp->parent = subst;  /* 保证后续修复沿正确的父链上溯 */

        } else {  /* 后继在更深处 */
            temp->parent = subst->parent;  /* temp 接替 subst 原位，父指针指向其旧父 */
        }  /* 结束 temp 父指针分派 */

        subst->left = node->left;  /* 开始结构搬移：接管 node 的左子树 */
        subst->right = node->right;  /* 接管 node 的右子树 */
        subst->parent = node->parent;  /* 接管 node 的父指针 */
        ngx_rbt_copy_color(subst, node);  /* 关键：继承 node 颜色——node 处黑高不变，失衡被转移到 subst 原位 */

        if (node == *root) {  /* 若删的是根，顶替者成为新根 */
            *root = subst;  /* 更新根指针 */

        } else {  /* 非根：父链对应一侧改指 subst */
            if (node == node->parent->left) {  /* node 原是左孩子 */
                node->parent->left = subst;  /* 接左 */
            } else {  /* node 原是右孩子 */
                node->parent->right = subst;  /* 接右 */
            }  /* 结束左右判断 */
        }  /* 结束根/非根重接 */

        if (subst->left != sentinel) {  /* 新孩子若非哨兵则回填父指针 */
            subst->left->parent = subst;  /* 左孩子认亲 */
        }  /* 结束左孩子回填 */

        if (subst->right != sentinel) {  /* 右侧同理 */
            subst->right->parent = subst;  /* 右孩子认亲 */
        }  /* 结束右孩子回填 */

        /* DEBUG stuff */
        node->left = NULL;  /* node 已逻辑删除：清空树指针 */
        node->right = NULL;  /* 防悬挂 */
        node->parent = NULL;  /* 防悬挂 */
        node->key = 0;  /* 键清零 */
    }  /* 结束 subst==node/搬移两大分支 */

    if (is_red) {  /* 摘走的是红节点：红不参与黑高计算，性质5未破坏 */
        return;  /* 无需修复，直接结束 */
    }  /* 结束红节点捷径 */

    /* a delete fixup */

    /* 循环不变式：temp 子树"少了一个黑"(黑亏空)；temp 为红时染黑即可补足，到根则自动消失 */
    while (temp != *root && ngx_rbt_is_black(temp)) {

        if (temp == temp->parent->left) {  /* temp 是左孩子：取右兄弟(右孩子情形为其镜像) */
            w = temp->parent->right;  /* w：兄弟节点，四种情形全部围绕它展开 */

            if (ngx_rbt_is_red(w)) {  /* 情形1：兄弟为红——旋转转成"黑兄弟"再按后续情形处理 */
                ngx_rbt_black(w);  /* 红兄染黑 */
                ngx_rbt_red(temp->parent);  /* 父染红：交换父子颜色 */
                ngx_rbtree_left_rotate(root, sentinel, temp->parent);  /* 左旋父：黑兄换到 temp 一侧且各路径黑高不变 */
                w = temp->parent->right;  /* 旋转后重新认领新兄弟(必为黑) */
            }  /* 结束情形1 */

            if (ngx_rbt_is_black(w->left) && ngx_rbt_is_black(w->right)) {  /* 情形2：两个侄子全黑 */
                ngx_rbt_red(w);  /* 兄染红：w 一侧黑高也 -1，与 temp 一侧"对齐" */
                temp = temp->parent;  /* 亏空上推给父：唯一能让循环继续上溯的情形 */

            } else {  /* 情形3/4：兄弟至少有一个红孩子，可旋转补足亏空 */
                if (ngx_rbt_is_black(w->right)) {  /* 情形3：红侄在内侧(w 左)——先转成情形4 的直线型 */
                    ngx_rbt_black(w->left);  /* 内侧红侄染黑 */
                    ngx_rbt_red(w);  /* w 染红：颜色随旋转就位 */
                    ngx_rbtree_right_rotate(root, sentinel, w);  /* 右旋 w：折线转直线，红侄移到外侧 */
                    w = temp->parent->right;  /* w 更新为新的(外侧带红侄的)兄弟 */
                }  /* 结束情形3 */

                ngx_rbt_copy_color(w, temp->parent);  /* 情形4：w 继承父色，准备接管父的位置 */
                ngx_rbt_black(temp->parent);  /* 父染黑：替 temp 一侧补上缺失的黑 */
                ngx_rbt_black(w->right);  /* 外侧红侄染黑：补偿旋转中被移走的一份黑 */
                ngx_rbtree_left_rotate(root, sentinel, temp->parent);  /* 左旋父：一次旋转终结全部亏空 */
                temp = *root;  /* 置 temp 为根：黑高已恢复，强制退出循环 */
            }  /* 结束情形2/3/4 */

        } else {  /* 镜像：temp 是右孩子，各情形左右对调 */
            w = temp->parent->left;  /* 左兄弟 */

            if (ngx_rbt_is_red(w)) {  /* 镜像情形1：红兄转黑兄 */
                ngx_rbt_black(w);  /* 兄染黑 */
                ngx_rbt_red(temp->parent);  /* 父染红 */
                ngx_rbtree_right_rotate(root, sentinel, temp->parent);  /* 右旋父 */
                w = temp->parent->left;  /* 新兄弟 */
            }  /* 结束镜像情形1 */

            if (ngx_rbt_is_black(w->left) && ngx_rbt_is_black(w->right)) {  /* 镜像情形2：双黑侄，亏空上推 */
                ngx_rbt_red(w);  /* 兄染红：两侧黑高同步 -1 */
                temp = temp->parent;  /* 以父为新亏空节点继续上溯 */

            } else {  /* 镜像情形3/4 */
                if (ngx_rbt_is_black(w->left)) {  /* 镜像情形3：红侄在内侧(w 右)——左旋 w 转直线型 */
                    ngx_rbt_black(w->right);  /* 内侧红侄染黑 */
                    ngx_rbt_red(w);  /* w 染红 */
                    ngx_rbtree_left_rotate(root, sentinel, w);  /* 左旋 w：红侄移到外侧 */
                    w = temp->parent->left;  /* 新兄弟 */
                }  /* 结束镜像情形3 */

                ngx_rbt_copy_color(w, temp->parent);  /* 镜像情形4：w 继承父色 */
                ngx_rbt_black(temp->parent);  /* 父染黑 */
                ngx_rbt_black(w->left);  /* 外侧红侄染黑 */
                ngx_rbtree_right_rotate(root, sentinel, temp->parent);  /* 右旋父：修复完成 */
                temp = *root;  /* 置根强制退出 */
            }  /* 结束镜像情形2/3/4 */
        }  /* 结束左/右镜像两大分支 */
    }  /* 删除修复循环结束 */

    ngx_rbt_black(temp);  /* 收尾：temp 若为红(情形2上推而来)染黑恰好补足亏空；为黑则幂等 */
}  /* ngx_rbtree_delete 结束 */


/*
 * ngx_rbtree_left_rotate()：以 node 为支点左旋，O(1) 仅改指针。
 *        node              temp
 *        /  \              /  \
 *       A   temp   ==>  node   C
 *           /  \        /  \
 *          B    C      A    B
 * B 的键恰介于 node 与 temp 之间，故过继后 BST 有序性保持。
 */
ngx_inline void ngx_rbtree_left_rotate(ngx_rbtree_t **root,
                                       ngx_rbtree_t *sentinel,  /* NIL 哨兵 */
                                       ngx_rbtree_t *node)  /* 旋转支点(将下沉) */
{  /* 函数体开始 */
    ngx_rbtree_t  *temp;  /* node 的右孩子：旋转后将升为子树根 */

    temp = node->right;  /* 记下右孩子 */
    node->right = temp->left;  /* temp 的左子树 B 过继给 node：B 键恰在两者之间 */

    if (temp->left != sentinel) {  /* B 非哨兵才需要回填父指针 */
        temp->left->parent = node;  /* B 认 node 为父 */
    }  /* 结束过继判断 */

    temp->parent = node->parent;  /* temp 顶替 node：先继承其父指针 */

    if (node == *root) {  /* node 是根：temp 成为新根 */
        *root = temp;  /* 更新根指针 */

    } else if (node == node->parent->left) {  /* node 是左孩子 */
        node->parent->left = temp;  /* 父的左指针改指 temp */

    } else {  /* node 是右孩子 */
        node->parent->right = temp;  /* 父的右指针改指 temp */
    }  /* 父链重接完毕 */

    temp->left = node;  /* node 降为 temp 的左孩子 */
    node->parent = temp;  /* 回填父指针，旋转闭环 */
}  /* ngx_rbtree_left_rotate 结束 */


/* 右旋：左旋的镜像——node 下沉为左孩子的右孩子，左孩子 temp 升为子树根 */
ngx_inline void ngx_rbtree_right_rotate(ngx_rbtree_t **root,
                                        ngx_rbtree_t *sentinel,  /* NIL 哨兵 */
                                        ngx_rbtree_t *node)  /* 旋转支点(将下沉) */
{  /* 函数体开始 */
    ngx_rbtree_t  *temp;  /* node 的左孩子：旋转后将升为子树根 */

    temp = node->left;  /* 记下左孩子 */
    node->left = temp->right;  /* temp 的右子树过继给 node：其键恰在两者之间 */

    if (temp->right != sentinel) {  /* 过继子树非哨兵才需回填 */
        temp->right->parent = node;  /* 过继子树认 node 为父 */
    }  /* 结束过继判断 */

    temp->parent = node->parent;  /* 继承 node 的父指针 */

    if (node == *root) {  /* node 是根：temp 升为新根 */
        *root = temp;  /* 更新根指针 */

    } else if (node == node->parent->right) {  /* node 是右孩子 */
        node->parent->right = temp;  /* 父的右指针改指 temp */

    } else {  /* node 是左孩子 */
        node->parent->left = temp;  /* 父的左指针改指 temp */
    }  /* 父链重接完毕 */

    temp->right = node;  /* node 降为 temp 的右孩子 */
    node->parent = temp;  /* 回填父指针，旋转闭环 */
}  /* ngx_rbtree_right_rotate 结束 */
