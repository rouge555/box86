#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "tools/bridge_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynablock.h"
#include "dynablock_private.h"
#include "dynarec_private.h"
#include "elfloader.h"
#ifdef ARM
#include "dynarec_arm.h"
#include "arm_lock_helper.h"
#else
#error Unsupported architecture!
#endif

#include "khash.h"

KHASH_MAP_INIT_INT(dynablocks, dynablock_t*)

uint32_t X31_hash_code(void* addr, int len)
{
    if(!len) return 0;
    uint8_t* p = (uint8_t*)addr;
	int32_t h = *p;
	for (--len, ++p; len; --len, ++p) h = (h << 5) - h + (int32_t)*p;
	return (uint32_t)h;
}

dynablocklist_t* NewDynablockList(uintptr_t base, uintptr_t text, int textsz, int nolinker, int direct)
{
    if(!textsz) {
        printf_log(LOG_NONE, "Error, creating a NULL sized Dynablock\n");
        return NULL;
    }
    dynablocklist_t* ret = (dynablocklist_t*)calloc(1, sizeof(dynablocklist_t));
    ret->base = base;
    ret->text = text;
    ret->textsz = textsz;
    ret->nolinker = nolinker;
    if(direct && textsz) {
        ret->direct = (dynablock_t**)calloc(textsz, sizeof(dynablock_t*));
        if(!ret->direct) {printf_log(LOG_NONE, "Warning, fail to create direct block for dynablock @%p\n", (void*)text);}
    }
    return ret;
}

void FreeDynablock(dynablock_t* db)
{
    if(db) {
        dynarec_log(LOG_DEBUG, "FreeDynablock(%p), db->block=%p x86=%p:%p father=%p, tablesz=%d, with %d son(s) already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size, db->father, db->tablesz, db->sons_size, db->gone);
        if(db->gone)
            return; // already in the process of deletion!
        db->done = 0;
        db->gone = 1;
        // remove from direct if there
        uintptr_t startdb = db->parent->text;
        uintptr_t enddb = db->parent->text + db->parent->textsz;
        if(db->parent->direct) {
            uintptr_t addr = (uintptr_t)db->x86_addr;
            if(addr>=startdb && addr<enddb)
                db->parent->direct[addr-startdb] = NULL;
        }
        // remove and free the sons
        for (int i=0; i<db->sons_size; ++i) {
            dynablock_t *son = (dynablock_t*)arm_lock_xchg(&db->sons[i], 0);
            FreeDynablock(son);
        }
        // only the father free the DynarecMap
        if(!db->father) {
            dynarec_log(LOG_DEBUG, " -- FreeDyrecMap(%p, %d)\n", db->block, db->size);
            FreeDynarecMap((uintptr_t)db->block, db->size);
        }
        free(db->sons);
        free(db->table);
        free(db->instsize);
        free(db);
    }
}

void FreeDynablockList(dynablocklist_t** dynablocks)
{
    if(!dynablocks)
        return;
    if(!*dynablocks)
        return;
    dynarec_log(LOG_DEBUG, "Free Direct Blocks %p from Dynablocklist nolinker=%d\n", (*dynablocks)->direct, (*dynablocks)->nolinker);
    if((*dynablocks)->direct) {
        for (int i=0; i<(*dynablocks)->textsz; ++i) {
            if((*dynablocks)->direct[i] && !(*dynablocks)->direct[i]->father) 
                FreeDynablock((*dynablocks)->direct[i]);
        }
        free((*dynablocks)->direct);
    }
    (*dynablocks)->direct = NULL;

    free(*dynablocks);
    *dynablocks = NULL;
}

void MarkDynablock(dynablock_t* db)
{
    if(db) {
        if(db->father)
            db = db->father;    // mark only father
        if(db->nolinker)
            db->need_test = 1;  // test only blocks that can be marked (and so deleted)
    }
}

void ProtectDynablock(dynablock_t* db)
{
    if(db) {
        if(db->father)
            return;    // protect only father, child(ren) will be automatically
        protectDB((uintptr_t)db->x86_addr, db->x86_size);
    }
}

void MarkDynablockList(dynablocklist_t** dynablocks)
{
    if(!dynablocks)
        return;
    if(!*dynablocks)
        return;
    if(!(*dynablocks)->nolinker)
        return;
    dynarec_log(LOG_DEBUG, "Marked Blocks from Dynablocklist nolinker=%d %p:0x%x\n", (*dynablocks)->nolinker, (void*)(*dynablocks)->text, (*dynablocks)->textsz);
    dynablock_t* db;
    if((*dynablocks)->direct) {
        for (int i=0; i<(*dynablocks)->textsz; ++i) {
            db = (*dynablocks)->direct[i];
            if(db)
                MarkDynablock(db);
        }
    }
}

void ProtectDynablockList(dynablocklist_t** dynablocks)
{
    if(!dynablocks)
        return;
    if(!*dynablocks)
        return;
    dynarec_log(LOG_DEBUG, "Protect Blocks from Dynablocklist nolinker=%d %p:0x%x\n", (*dynablocks)->nolinker, (void*)(*dynablocks)->text, (*dynablocks)->textsz);
    dynablock_t* db;
    if((*dynablocks)->direct) {
        for (int i=0; i<(*dynablocks)->textsz; ++i) {
            db = (*dynablocks)->direct[i];
            if(db)
                ProtectDynablock(db);
        }
    }
}

uintptr_t StartDynablockList(dynablocklist_t* db)
{
    if(db)
        return db->text;
    return 0;
}
uintptr_t EndDynablockList(dynablocklist_t* db)
{
    if(db)
        return db->text+db->textsz-1;
    return 0;
}
void MarkDirectDynablock(dynablocklist_t* dynablocks, uintptr_t addr, uintptr_t size)
{
    if(!dynablocks)
        return;
    if(!dynablocks->nolinker)
        return;
    uintptr_t startdb = dynablocks->text;
    uintptr_t enddb = startdb + dynablocks->textsz;
    uintptr_t start = addr;
    uintptr_t end = addr+size;
    if(start<startdb)
        start = startdb;
    if(end>enddb)
        end = enddb;
    if(end>startdb && start<enddb)
        for(uintptr_t i = start; i<end; ++i)
            if(dynablocks->direct[i-startdb]) {
                MarkDynablock(dynablocks->direct[i-startdb]);
            }
}

void ProtectDirectDynablock(dynablocklist_t* dynablocks, uintptr_t addr, uintptr_t size)
{
    if(!dynablocks)
        return;
    uintptr_t startdb = dynablocks->text;
    uintptr_t enddb = startdb + dynablocks->textsz;
    uintptr_t start = addr;
    uintptr_t end = addr+size;
    if(start<startdb)
        start = startdb;
    if(end>enddb)
        end = enddb;
    if(end>startdb && start<enddb)
        protectDB(start, end-start); //no +1; as end/enddb is exclusive and not inclusive
}

void FreeRangeDynablock(dynablocklist_t* dynablocks, uintptr_t addr, uintptr_t size)
{
    if(!dynablocks)
        return;

    if(dynablocks->direct) {
        dynablock_t* db;
        int ret;
        khint_t k;
        kh_dynablocks_t *blocks = kh_init(dynablocks);
        // copy in a temporary list
        if(dynablocks->direct) {
            uintptr_t startdb = dynablocks->text;
            uintptr_t enddb = startdb + dynablocks->textsz;
            uintptr_t start = addr;
            uintptr_t end = addr+size;
            if(start<startdb)
                start = startdb;
            if(end>enddb)
                end = enddb;
            if(end>startdb && start<enddb)
                for(uintptr_t i = start; i<end; ++i) {
                    db = (dynablock_t*)arm_lock_xchg(&dynablocks->direct[i-startdb], 0);
                    if(db) {
                        if(db->father)
                            db = db->father;
                        k = kh_put(dynablocks, blocks, (uintptr_t)db, &ret);
                        kh_value(blocks, k) = db;
                    }
                }
        }
        // purge the list
        kh_foreach_value(blocks, db,
            FreeDynablock(db);
        );
        kh_destroy(dynablocks, blocks);
    }
}
void MarkRangeDynablock(dynablocklist_t* dynablocks, uintptr_t addr, uintptr_t size)
{
    if(!dynablocks)
        return;
    if(!dynablocks->nolinker)
        return;
    if(dynablocks->direct)
        MarkDirectDynablock(dynablocks, addr, size);
}

dynablock_t* FindDynablockDynablocklist(void* addr, dynablocklist_t* dynablocks)
{
    if(!dynablocks)
        return NULL;
    if(dynablocks->direct)
        for(int i=0; i<dynablocks->textsz; ++i) {
            dynablock_t* db;
            if((db = dynablocks->direct[i])) {
                uintptr_t s = (uintptr_t)db->block;
                uintptr_t e = (uintptr_t)db->block+db->size;
                if((uintptr_t)addr>=s && (uintptr_t)addr<e)
                    return db->father?db->father:db;
            }
        }
    return NULL;
}

dynablock_t* FindDynablockFromNativeAddress(void* addr)
{
    // unoptimized search through all dynablockslist for the dynablock that contains native addr (NULL if not found)
    dynablock_t *ret = NULL;
    for(int idx=0; idx<DYNAMAP_SIZE && !ret; ++idx)
        if(my_context->dynmap[idx])
            ret = FindDynablockDynablocklist(addr, my_context->dynmap[idx]->dynablocks);
    if(ret)
        return ret;
    return ret;
}

static dynablocklist_t* getDBFromAddress(uintptr_t addr)
{
    int idx = (addr>>DYNAMAP_SHIFT);
    if(!my_context->dynmap[idx]) {
        return NULL;
    }
    return my_context->dynmap[idx]->dynablocks;
}

dynablock_t *AddNewDynablock(dynablocklist_t* dynablocks, uintptr_t addr, int* created)
{
    if(!dynablocks) {
        dynarec_log(LOG_INFO, "Warning: Ask to create a dynablock with a NULL dynablocklist (addr=%p)\n", (void*)addr);
        *created = 0;
        return NULL;
    }
    if((addr<dynablocks->text) || (addr>=(dynablocks->text+dynablocks->textsz))) {
        // this should be useless
        //dynarec_log(LOG_INFO, "Warning: Refused to create a Direct Block that is out-of-bound: dynablocks=%p (%p:%p), addr=%p\n", dynablocks, (void*)(dynablocks->text), (void*)(dynablocks->text+dynablocks->textsz), (void*)addr);
        //*created = 0;
        //return NULL;
        return AddNewDynablock(getDBFromAddress(addr), addr, created);
    }
    dynablock_t* block = NULL;
    // first, check if it exist in direct access mode
    if(dynablocks->direct) {
        block = dynablocks->direct[addr-dynablocks->text];
        if(block) {
            dynarec_log(LOG_DUMP, "Block already exist in Direct Map\n");
            *created = 0;
            return block;
        }
    }
    
    if (!*created)
        return block;
    
    if(!dynablocks->direct)
        dynablocks->direct = (dynablock_t**)calloc(dynablocks->textsz, sizeof(dynablock_t*));

    // create and add new block
    dynarec_log(LOG_DUMP, "Ask for DynaRec Block creation @%p\n", (void*)addr);

    block = (dynablock_t*)calloc(1, sizeof(dynablock_t));
    dynablock_t* tmp = (dynablock_t*)arm_lock_storeifnull(&dynablocks->direct[addr-dynablocks->text], block);
    if(tmp !=  block) {
        // a block appeard!
        free(block);
        *created = 0;
        return tmp;
    }

    block->parent = dynablocks;

    *created = 1;
    return block;
}

/* 
    return NULL if block is not found / cannot be created. 
    Don't create if create==0
*/
static dynablock_t* internalDBGetBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr, int create, dynablock_t* current)
{
    // try the quickest way first: get parent of current and check if ok!
    dynablocklist_t *dynablocks = NULL;
    dynablock_t* block = NULL;
    if(current) {
        dynablocks = current->parent;    
        if(!(addr>=dynablocks->text && addr<(dynablocks->text+dynablocks->textsz)))
            dynablocks = NULL;
        else if(dynablocks->direct && (addr>=dynablocks->text) && (addr<(dynablocks->text+dynablocks->textsz))) {
            block = dynablocks->direct[addr-dynablocks->text];
            if(block)
                return block;
        }
    }
    // nope, lets do the long way
    if(!dynablocks)
        dynablocks = getDBFromAddress(addr);
    if(!dynablocks)
        dynablocks = GetDynablocksFromAddress(emu->context, addr);
    if(!dynablocks)
        return NULL;
    // check direct first, without lock
    if(dynablocks->direct && (addr>=dynablocks->text) && (addr<(dynablocks->text+dynablocks->textsz)))
        block = dynablocks->direct[addr-dynablocks->text];
    if(block)
        return block;

    int created = create;
    block = AddNewDynablock(dynablocks, addr, &created);
    if(!created)
        return block;   // existing block...

    if(box86_dynarec_dump)
        pthread_mutex_lock(&my_context->mutex_dyndump);
    // fill the block
    block->x86_addr = (void*)addr;
    FillBlock(block, filladdr);
    if(box86_dynarec_dump)
        pthread_mutex_unlock(&my_context->mutex_dyndump);

    dynarec_log(LOG_DEBUG, " --- DynaRec Block %s @%p:%p (%p, 0x%x bytes, with %d son(s))\n", created?"created":"recycled", (void*)addr, (void*)(addr+block->x86_size), block->block, block->size, block->sons_size);

    return block;
}

dynablock_t* DBGetBlock(x86emu_t* emu, uintptr_t addr, int create, dynablock_t** current)
{
    dynablock_t *db = internalDBGetBlock(emu, addr, addr, create, *current);
    if(db && (db->need_test || (db->father && db->father->need_test))) {
        dynablock_t *father = db->father?db->father:db;
        uint32_t hash = father->nolinker?X31_hash_code(father->x86_addr, father->x86_size):0;
        if(hash!=father->hash) {
            dynarec_log(LOG_DEBUG, "Invalidating block %p from %p:%p (hash:%X/%X) with %d son(s)\n", father, father->x86_addr, father->x86_addr+father->x86_size, hash, father->hash, father->sons_size);
            // no more current if it gets invalidated too
            if(*current && father->x86_addr>=(*current)->x86_addr && (father->x86_addr+father->x86_size)<(*current)->x86_addr)
                *current = NULL;
            // Free father, it's now invalid!
            FreeDynablock(father);
            // start again... (will create a new block)
            db = internalDBGetBlock(emu, addr, addr, create, *current);
        } else {
            father->need_test = 0;
            protectDB((uintptr_t)father->x86_addr, father->x86_size);
        }
    } 
    return db;
}

dynablock_t* DBAlternateBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr)
{
    dynarec_log(LOG_DEBUG, "Creating AlternateBlock at %p for %p\n", (void*)addr, (void*)filladdr);
    int create = 1;
    dynablock_t *db = internalDBGetBlock(emu, addr, filladdr, create, NULL);
    if(db && (db->need_test || (db->father && db->father->need_test))) {
        dynablock_t *father = db->father?db->father:db;
        uint32_t hash = father->nolinker?X31_hash_code(father->x86_addr, father->x86_size):0;
        if(hash!=father->hash) {
            dynarec_log(LOG_DEBUG, "Invalidating block %p from %p:%p (hash:%X/%X) with %d son(s)\n", father, father->x86_addr, father->x86_addr+father->x86_size, hash, father->hash, father->sons_size);
            // Free father, it's now invalid!
            FreeDynablock(father);
            // start again... (will create a new block)
            db = internalDBGetBlock(emu, addr, filladdr, create, NULL);
        } else {
            father->need_test = 0;
            protectDB((uintptr_t)father->x86_addr, father->x86_size);
        }
    } 
    return db;
}
