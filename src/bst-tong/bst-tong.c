#include "bst-tong.h"

RETRY_STATS_VARS;

__thread seek_record_t* seek_record;
__thread ssmem_allocator_t* alloc;

node_t* initialize_tree(){
    node_t* r;
    node_t* s;
    node_t* inf0;
    node_t* inf1;
    node_t* inf2;
    r = create_node(INF2,0,1);
    s = create_node(INF1,0,1);
    inf0 = create_node(INF0,0,1);
    inf1 = create_node(INF1,0,1);
    inf2 = create_node(INF2,0,1);
    
    asm volatile("" ::: "memory");
    r->left = s;
    r->right = inf2;
    s->right = inf1;
    s->left= inf0;
    asm volatile("" ::: "memory");

    return r;
}

void bst_init_local() {
  seek_record = (seek_record_t*) memalign(CACHE_LINE_SIZE, sizeof(seek_record_t));
  assert(seek_record != NULL);
}

node_t* create_node(skey_t k, sval_t value, int initializing) {
    volatile node_t* new_node;
#if GC == 1
    if (unlikely(initializing)) {
        new_node = (volatile node_t*) ssalloc_aligned(CACHE_LINE_SIZE, sizeof(node_t));
    } else {
        new_node = (volatile node_t*) ssmem_alloc(alloc, sizeof(node_t));
    }
#else 
    new_node = (volatile node_t*) ssalloc(sizeof(node_t));
#endif
    if (new_node == NULL) {
        perror("malloc in bst create node");
        exit(1);
    }
    new_node->left = NULL;
    new_node->right = NULL;
    new_node->key = k;
    new_node->value = value;
    new_node->marked =0;
    INIT_LOCK(&(new_node->lock));

    asm volatile("" ::: "memory");
    return (node_t*) new_node;
}


seek_record_t * bst_seek(skey_t key, node_t* node_r){
  PARSE_TRY();
    volatile seek_record_t seek_record_l;
    node_t* node_s = node_r->left;
    seek_record_l.gp = node_r;
    seek_record_l.p = node_s; 
    seek_record_l.n = node_s->left;

    node_t* current = seek_record_l.n->left;
    
    while (likely(current!=NULL)) {
        seek_record_l.gp = seek_record_l.p;
        seek_record_l.p = seek_record_l.n;
        seek_record_l.n = current;

        if (key < current->key) {
            current =  current->left;
        } else {
            current =  current->right;
        }
    }
    seek_record->gp=seek_record_l.gp;
    seek_record->p=seek_record_l.p;
    seek_record->n=seek_record_l.n;
    return seek_record;
}

sval_t bst_search(skey_t key, node_t* node_r) {
   bst_seek(key, node_r);
   if (seek_record->n->key == key) {
        return seek_record->n->value;
   } else {
        return 0;
   }
}


bool_t bst_insert(skey_t key, sval_t val, node_t* node_r) {
    node_t* new_internal = NULL;
    node_t* new_node = NULL;
    uint created = 0;
    while (1) {
      UPDATE_TRY();

        bst_seek(key, node_r);
        if (seek_record->n->key == key) {
#if GC == 1
            if (created) {
                ssmem_free(alloc, new_internal);
                ssmem_free(alloc, new_node);
            }
#endif
            return FALSE;
        }
        node_t* parent = seek_record->p;
        node_t* leaf = seek_record->n;

                //TODO check this
        if (likely(created==0)) {
            new_internal=create_node(max(key,leaf->key),0,0);
            new_node = create_node(key,val,0);
            created=1;
        } else {
            new_internal->key=max(key,leaf->key);
        }
        if ( key < leaf->key) {
            new_internal->left = new_node;
            new_internal->right = leaf; 
        } else {
            new_internal->right = new_node;
            new_internal->left = leaf;
        }
 #ifdef __tile__
    MEM_BARRIER;
#endif

        LOCK(&(parent->lock));
        LOCK(&(leaf->lock));
        if (!(parent->marked) && ((leaf==parent->left) || (leaf==parent->right))) {
            if (key < parent->key) {
	            parent->left = new_internal;
            } else {
	            parent->right =  new_internal;
            }
            UNLOCK(&(parent->lock));
           UNLOCK(&(leaf->lock));
            return TRUE; 
        }
       UNLOCK(&(parent->lock));
       UNLOCK(&(leaf->lock));
    }
}

inline int is_parent (node_t* p, node_t* c) {
    if ((p->left == c) || (p->right == c)) return 1;
    return 0;
}

sval_t bst_remove(skey_t key, node_t* node_r) {
    node_t* leaf;
    sval_t val = 0;
    while (1) {
      UPDATE_TRY();

        bst_seek(key, node_r);
        leaf = seek_record->n;
        if (leaf->key != key) {
                return 0;
        }
        val = seek_record->n->value;
        node_t* parent = seek_record->p;
        node_t* gp = seek_record->gp;

        LOCK(&(gp->lock));
        LOCK(&(parent->lock));
        LOCK(&(leaf->lock));
        if (leaf->marked) {
            UNLOCK(&(gp->lock));
            UNLOCK(&(parent->lock));
            UNLOCK(&(leaf->lock));

            return 0;
        }
        if (is_parent(gp,parent) && is_parent(parent,leaf) && (gp->marked==0)){
            leaf->marked = 1;
            parent->marked = 1;
            node_t* other;
            if (parent->left==leaf) {
                other=parent->right;
            } else {
                other= parent->left;
            }
            if (gp->left==parent) {
                gp->left = other;
            } else {
                gp->right = other;
            }
            UNLOCK(&(gp->lock));
            UNLOCK(&(parent->lock));
            UNLOCK(&(leaf->lock));
#if GC == 1
            ssmem_free(alloc, parent);
            ssmem_free(alloc, leaf);
#endif

            return val;
        }
        UNLOCK(&(gp->lock));
        UNLOCK(&(parent->lock));
        UNLOCK(&(leaf->lock));
    }
}



uint32_t bst_size(volatile node_t* node) {
    if (node == NULL) return 0; 
    if ((node->left == NULL) && (node->right == NULL)) {
       if (node->key < INF0 ) return 1;
    }
    uint32_t l = 0;
    uint32_t r = 0;
    if ((node->left!=NULL) &&  !(node->left->marked)) {
        l = bst_size(node->left);
    }
    if ((node->right!=NULL) &&  !(node->right->marked)) {
        r = bst_size(node->right);
    }
    return l+r;
}

