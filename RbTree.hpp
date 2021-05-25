#pragma once

#include <cstdint>

#define ARENA
//#define TEST

#ifdef TEST
    #include <stdlib.h>
#endif

#ifdef ARENA
    #define EMBED
#endif

inline void dalloc(void* ptr){
#ifdef TEST
    free(ptr);
#endif
}

inline void* alloc(std::size_t size){
#ifdef TEST
    return malloc(size);
#else
    return nullptr;
#endif
}

static void (*mem_dalloc)(void*) = dalloc;
static void* (*mem_alloc)(std::size_t) = alloc;

enum direction{
    left,right,nil
};

template<typename T>
struct rb_tree;

template<typename T>
struct rbnode{
    T* ptr;
    bool is_red;
    struct rb_tree<T>* tree;
    struct rbnode* father;
    struct rbnode* lson;
    struct rbnode* rson;
};

template<typename T>
using rbnode_t = struct rbnode<T>;

template<typename T>
struct rb_tree{
    rbnode_t<T>* root;
	
    int size;

    bool (*_bigger)(const T*,const T*);

    bool (*_equal)(const T*,const T*);
};

template<typename T>
using rb_tree_t = struct rb_tree<T>;

template<typename T>
struct rb_iter{    
    rb_tree_t<T>* tree;
    rbnode_t<T>* now;
    rbnode_t<T>* prev;

    rb_iter(rb_tree_t<T>* tree)
        :tree(tree)
    {   
        if (tree->root==nullptr){
            now = nullptr;
            prev = nullptr;
            return;
        }
        //now = tree->leftmost;
        now = tree->root;
        while (now->lson!=nullptr)
            now = now->lson;
        prev = now;
    }

    void reset(rb_tree_t<T>* tree){
        now = tree->root;
        while (now->lson!=nullptr)
            now = now->lson;
    }

    void iter_del(){
        if (prev==nullptr)
            return;
        if (now==nullptr){
            if (prev!=nullptr)
                rb_delete(tree,prev);
            return;
        }
        T* tptr = now->ptr;
        rb_delete(tree,prev);
        rbnode_t<T> snode;
        rbnode_init(&snode,tptr,true);
        now = rb_search(tree,&snode);
    }

    // at a great chance this is slow
    rbnode_t<T>* next(){
        if (now==nullptr)
            return nullptr;
        prev = now;
        if (now->rson!=nullptr){
            now = now->rson;
            while (now->lson!=nullptr)
                now = now->lson;
        }else if (now->father==nullptr){
            now = nullptr;
        }else if (now->father->lson!=nullptr && equal(now->father->lson,now)){
            now = now->father;
        }else{
            while (now->father!= nullptr && now->father->rson!=nullptr && equal(now->father->rson,now)){
                now = now->father;
            }
            now = now->father;
        }
        return prev;
    }

    T* next_ptr(){
        rbnode_t<T>* n = next();
        if (n==nullptr)
            return nullptr;
        return n->ptr;
    }
};

template<typename T>
using rb_iter_t = struct rb_iter<T>;

template<typename T>
inline bool bigger(rbnode_t<T>* a,rbnode_t<T>* b){
    struct rb_tree<T>* tr = (a->tree==nullptr)?b->tree:a->tree;
    return tr->_bigger(a->ptr,b->ptr);
}

template<typename T>
inline bool equal(rbnode_t<T>* a,rbnode_t<T>* b){
    struct rb_tree<T>* tr = (a->tree==nullptr)?b->tree:a->tree;
    return tr->_equal(a->ptr,b->ptr);
}

template<typename T>
inline void rbnode_init(rbnode_t<T>* node,T* ptr,bool is_red){
    node->tree = nullptr;
    node->father = nullptr;
    node->lson = nullptr;
    node->rson = nullptr;
    node->is_red = is_red;
    node->ptr = ptr;
}

template<typename T>
inline void swap_ptr(rbnode_t<T>* a,rbnode_t<T>* b){
#ifdef EMBED
    direction which = nil;
    if (b==a->lson)
        which = left;
    else if (b==a->rson)
        which = right;
    
    rbnode_t<T>* afather = a->father;
    rbnode_t<T>* bfather = b->father;
    direction ad = nil;
    if (afather!=nullptr)
        ad = (a==afather->lson)?left:right;
    rbnode_t<T>* blson = b->lson;
    rbnode_t<T>* brson = b->rson;
    if (which!=nil){
        if (which==left){
            rbnode_t<T>* arson = a->rson;
            b->lson = a;
            b->rson = arson;
            if (arson!=nullptr)
                arson->father = b;
        }else{
            rbnode_t<T>* alson = a->lson;
            b->rson = a;
            b->lson = alson;
            if (alson!=nullptr)
                alson->father = b;
        }
        b->father = afather;
        if (ad==left)
            afather->lson = b;
        else if (ad==right)
            afather->rson = b;

        a->father = b;
        a->lson = blson;
        a->rson = brson;
    }else{
        rbnode_t<T>* alson = a->lson;
        rbnode_t<T>* arson = a->rson;

        direction bd = nil;
        if (bfather!=nullptr)
            bd = (b==bfather->lson)?left:right;

        if (ad==left)
            afather->lson = b;
        else if (ad==right)
            afather->rson = b;
        b->father = afather;

        if (bd==left)
            bfather->lson = a;
        else if (bd==right)
            bfather->rson = a;
        a->father = bfather;

        if (alson!=nullptr)
            alson->father = b;
        if (arson!=nullptr)
            arson->father = b;
        
        if (blson!=nullptr)
            blson->father = a;
        if (brson!=nullptr)
            brson->father = a;

        a->lson = blson;
        a->rson = brson;

        b->lson = alson;
        b->rson = arson;
    }

    bool ared = a->is_red;
    a->is_red = b->is_red;
    b->is_red = ared;

    rb_tree_t<T>* tree = a->tree;
    if (tree==nullptr)
        return;
    if (a==tree->root)
        tree->root = b;
    else if (b==tree->root)
        tree->root = a;
#else
    T* tmp = a->ptr;
    a->ptr = b->ptr;
    b->ptr = tmp;
#endif
}

template<typename T>
inline rbnode_t<T>* new_rbnode(T* ptr,bool is_red){
    rbnode_t<T>* node = (rbnode_t<T>*)mem_alloc(sizeof(rbnode_t<T>));
    if (node==nullptr) 
        return nullptr;
    rbnode_init(node,ptr,is_red);
    return node;
}

template<typename T>
inline void rotate_left(rbnode_t<T>* node){
    if (node->father==nullptr)
        return;
    rbnode_t<T>* father = node->father;
    rbnode_t<T>* gfather = father->father;
    rbnode_t<T>* lson = node->lson;
    father->rson = lson;
    if (lson!=nullptr)
        lson->father = father;
    node->lson = father;
    node->father = gfather;
    if (gfather!=nullptr){
        if (gfather->lson==father)
            gfather->lson = node;
        else
            gfather->rson = node;
    }
    father->father = node;
}

template<typename T>
inline void rotate_right(rbnode_t<T>* node){
    if (node->father==nullptr)
        return;
    rbnode_t<T>* father = node->father;
    rbnode_t<T>* gfather = father->father;
    rbnode_t<T>* rson = node->rson;
    father->lson = rson;
    if (rson!=nullptr)
        rson->father = father;
    node->rson = father;
    node->father = gfather;
    if (gfather!=nullptr){
        if (gfather->lson==father)
            gfather->lson = node;
        else
            gfather->rson = node;
    }
    father->father = node;
}

template<typename T>
inline rbnode_t<T>* rebalance_fast(rbnode_t<T>* root,rbnode_t<T>* node){
    while (node!=root && node->is_red && node->father->is_red){
        rbnode_t<T>* father = node->father;
        rbnode_t<T>* gfather = father->father;
        rbnode_t<T>* uncle = nullptr;

        // which son of grandfather is father
        direction which_son = nil;

        if (gfather!=nullptr){
            if (gfather->lson==father){
                uncle = gfather->rson;
                which_son = left;
            }
            else{
                uncle = gfather->lson;
                which_son = right;
            }
        }
    
        if (which_son==left && node==father->rson){
            rotate_left(node);
            father = gfather->lson;
        }
        else if (which_son==right && node==father->lson){
            rotate_right(node);
            father = gfather->rson;
        }

        if (uncle!=nullptr && uncle->is_red){
            father->is_red = false;
            uncle->is_red = false;
            if (gfather->father!=nullptr)
                gfather->is_red = true;
            node = gfather;
            continue;
        }

        father->is_red = false;
        gfather->is_red = true;

        if (which_son==left){
            father->lson->is_red = true;
            rotate_right(father);
        }
        else{ 
            father->rson->is_red = true;
            rotate_left(father);
        } 
        if (gfather==root)
            root = father;
        node = father;
    }
    root->is_red = false;
    return root;
}

template<typename T>
inline rbnode_t<T>* insert_fast(rb_tree_t<T>* tree,rbnode_t<T>* node){
	rbnode_t<T>* root = tree->root;
	if (root==nullptr){
        node->is_red = false;
        root = node;
        return root;
    }
    rbnode_t<T>* tr = root;
    for (;;){
        if (bigger(tr,node)){
            if (tr->lson==nullptr){
                tr->lson = node;
                node->father = tr;
                return rebalance_fast(root,node);
            }
            tr = tr->lson;
        }
        else{
            if (tr->rson==nullptr){
                tr->rson = node;
                node->father = tr;
                return rebalance_fast(root,node);
            }
            tr = tr->rson;
        }
    }
}

template<typename T>
inline rbnode_t<T>* search(rbnode_t<T>* root,rbnode_t<T>* node){
    while (root!=nullptr){
        if (equal(root,node))
            return root;
        else if (bigger(root,node))
            root = root->lson;
        else
            root = root->rson;
    }
    return root;
}

template<typename T>
inline rbnode_t<T>* search_maybe_bigger(rbnode_t<T>* root,rbnode_t<T>* node){
	rbnode_t<T>* ret = nullptr;
	while (root!=nullptr){
        if (equal(root,node)){
        	ret = root;
        	break;
        }
        else if (bigger(root,node)){
        	ret = root;
        	root = root->lson;
        }
        else
            root = root->rson;
    }
    return ret;
}

template<typename T>
inline rbnode_t<T>* right_son_smallest(rbnode_t<T>* node){
    if (node->rson==nullptr)
        return nullptr;
    rbnode_t<T>* new_root = node->rson;
    while (new_root->lson!=nullptr)
        new_root = new_root->lson;
    return new_root;
}

template<typename T>
inline rbnode_t<T>* left_son_biggest(rbnode_t<T>* node){
    if (node->lson==nullptr)
        return nullptr;
    rbnode_t<T>* new_root = node->lson;
    while (new_root->rson!=nullptr)
        new_root = new_root->rson;
    return new_root;
}

template<typename T>
inline bool is_leaf(rbnode_t<T>* root){
    return root->lson==nullptr && root->rson==nullptr;
}

template<typename T>
inline bool no_red_child(rbnode_t<T>* node){
    if (node->lson!=nullptr && node->lson->is_red)
        return false;
    if (node->rson!=nullptr && node->rson->is_red)
        return false;
    return true;
}

template<typename T>
inline rbnode_t<T>* del_helper(rbnode_t<T>* root,rbnode_t<T>* node){
    while (node!=root && !node->is_red){
        rbnode_t<T>* father = node->father;
        direction which_son = (node==father->lson)?left:right;
        if (which_son==left){
            rbnode_t<T>* brother = father->rson;
            if (brother->is_red){
                father->is_red = true;
                brother->is_red = false;
                if (father==root)
                    root = root->rson;
                rotate_left(brother);
                continue;
            }
            if (no_red_child(brother)){
                brother->is_red = true;
                node = father;
                continue;
            }
            if (brother->lson!=nullptr && (brother->rson==nullptr||!brother->rson->is_red)){
                brother->lson->is_red = false;
                brother->is_red = true;
                rotate_right(brother->lson);
                brother = father->rson;
            }
            brother->is_red = father->is_red;
            brother->rson->is_red = false;
            father->is_red = false;
            if (father==root)
                root = root->rson;
            rotate_left(brother);
            break;
        }
        else{
            rbnode_t<T>* brother = father->lson;
            if (brother->is_red){
                father->is_red = true;
                brother->is_red = false;
                if (father==root)
                    root = root->lson;
                rotate_right(brother);
                continue;
            }
            if (no_red_child(brother)){
                brother->is_red = true;
                node = father;
                continue;
            }
            if (brother->rson!=nullptr&&(brother->lson==nullptr||!brother->lson->is_red)){
                brother->rson->is_red = false;
                brother->is_red = true;
                rotate_left(brother->rson);
                brother = father->lson;
            }
            brother->is_red = father->is_red;
            brother->lson->is_red = false;
            father->is_red = false;
            if (father==root)
                root = root->lson;
            rotate_right(brother);
            break;
        }
    }
    node->is_red = false;
    return root;
}

template<typename T>
inline rbnode_t<T>* _delete(rbnode_t<T>* root,rbnode_t<T>* node){
    rbnode_t<T>* target = search(root,node);
    if (target==nullptr)
        return root;

    direction which_son = nil;
    if (target->father!=nullptr){
        if (target->father->lson == target)
            which_son = left;
        else
            which_son = right;
    }

    if (is_leaf(target) && (target->is_red || which_son==nil)){  
        if (which_son==left)
            target->father->lson = nullptr;
        else if (which_son==right)
            target->father->rson = nullptr;
        else
            root = nullptr;
        mem_dalloc(target);
        return root;
    }

    if (target->lson!=nullptr && target->rson==nullptr){
        rbnode_t<T>* tlson = target->lson;
        swap_ptr(target,tlson);
    #ifdef EMBED
        if (root==target)
            root = tlson;
        if (!target->is_red)
            root = del_helper(root,target);
        mem_dalloc(target);
        tlson->lson = nullptr;
    #else
        if (!target->lson->is_red){
            root = del_helper(root,target->lson);
            //return _delete(root,target->lson);
        }
        mem_dalloc(target->lson);
        target->lson = nullptr;
    #endif
        return root;
    }
    else if (target->rson!=nullptr && target->lson==nullptr){
        rbnode_t<T>* trson = target->rson;
        swap_ptr(target,trson);
    #ifdef EMBED
        if (target==root)
            root = trson;
        if (!target->is_red)
            root = del_helper(root,target);
        mem_dalloc(target);
        trson->rson = nullptr;
    #else
        if (!target->rson->is_red){
            root = del_helper(root,target->rson);
            //return _delete(root,target->lson);
        }
        mem_dalloc(target->rson);
        target->rson = nullptr;
    #endif
        return root;
    }
    else if (target->lson!=nullptr && target->rson!=nullptr){
        rbnode_t<T>* tmp = right_son_smallest(target);
        swap_ptr(target,tmp);
    #ifdef EMBED
        if (target==root)
            root = tmp;
        if (target->rson!=nullptr){
            rbnode_t<T>* trson = target->rson;
            swap_ptr(target,trson);
            if (root==target)
                root = trson;
            if (target->is_red){
                trson->rson = nullptr;
                mem_dalloc(target);
                return root;
            }
        }
    #else
        if (tmp->rson!=nullptr){
            swap_ptr(tmp,tmp->rson);
            if (tmp->rson->is_red){
                tmp->rson = nullptr;
                mem_dalloc(tmp->rson);
                return root;
            }
            else
                tmp = tmp->rson;
            //return _delete(root,tmp->rson);
        }
        target = tmp;
    #endif
    }

    root = del_helper(root,target);

    if (target==target->father->lson)
        target->father->lson = nullptr;
    else
        target->father->rson = nullptr;

    mem_dalloc((void*)target);
    return root;
}

template<typename T>
inline void destroy(rbnode_t<T>* root){
    if (root==nullptr)
        return;
    if (root->lson!=nullptr)
        destroy(root->lson);
    if (root->rson!=nullptr)
        destroy(root->rson);
    mem_dalloc((void*)root);
    return;
}

template<typename T>
inline bool rb_empty(rb_tree_t<T>* tree){
    return tree->root == nullptr;
}

template<typename T>
inline void rb_init(rb_tree_t<T>* tree,
            bool (*bigger0)(const T*,const T*),bool (*equal0)(const T*,const T*))
{   
    tree->size = 0;
    tree->root = nullptr;
    tree->_bigger = bigger0;
    tree->_equal = equal0;
}

template<typename T>
inline void rb_insert(rb_tree_t<T>* tree,rbnode_t<T>* node){
    node->tree = tree;
    node->is_red = true;
#ifdef ARENA
    node->father = nullptr;
    node->lson = nullptr;
    node->rson = nullptr;
#endif
    tree->root = insert_fast(tree,node);
    ++tree->size;
}

template<typename T>
inline void rb_delete(rb_tree_t<T>* tree,rbnode_t<T>* node){
    tree->root = _delete(tree->root,node);
    --tree->size;
#ifdef ARENA
    node->father = nullptr;
    node->lson = nullptr;
    node->rson = nullptr;
    node->tree = nullptr;
#endif
}

template<typename T>
inline rbnode_t<T>* rb_search(rb_tree_t<T>* tree,rbnode_t<T>* node){
    return search(tree->root,node);
}

template<typename T>
inline rbnode_t<T>* rb_fsearch(rb_tree_t<T>* tree,rbnode_t<T>* node){
	return search_maybe_bigger(tree->root,node);
}
