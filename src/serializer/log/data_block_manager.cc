#include "data_block_manager.hpp"
#include "log_serializer.hpp"
#include "utils.hpp"
#include "arch/arch.hpp"

void data_block_manager_t::start(direct_file_t *file) {

    assert(state == state_unstarted);
    dbfile = file;
    last_data_extent = NULL;

    state = state_ready;
}

void data_block_manager_t::start(direct_file_t *file, metablock_mixin_t *last_metablock) {

    assert(state == state_unstarted);
    dbfile = file;
    if (last_metablock->last_data_extent) {
        last_data_extent = new gc_entry(last_metablock);
    } else {
        last_data_extent = NULL;
    }
    state = state_ready;
}

bool data_block_manager_t::read(off64_t off_in, void *buf_out, iocallback_t *cb) {

    assert(state == state_ready);

    dbfile->read_async(off_in, block_size, buf_out, cb);

    return false;
}

bool data_block_manager_t::write(void *buf_in, off64_t *off_out, iocallback_t *cb) {
    // Either we're ready to write, or we're shutting down and just
    // finished reading blocks for gc and called do_write.
    assert(state == state_ready
           || (state == state_shutting_down && gc_state.step == gc_read));

    off64_t offset = *off_out = gimme_a_new_offset();

    dbfile->write_async(offset, block_size, buf_in, cb);

    return false;
}

void data_block_manager_t::mark_garbage(off64_t offset) {

    unsigned int extent_id = (offset / extent_manager->extent_size);
    unsigned int block_id = (offset % extent_manager->extent_size) / block_size;
    
    gc_entry *entry = entries.get(extent_id);
    assert(entry->g_array[block_id] == 0);
    entry->g_array.set(block_id, 1);
    
    if (entry->state != gc_entry::state_young) {
        gc_stats.unyoung_garbage_blocks++;
    }
    
    if (entry->g_array.count() == entry->g_array.size()) {
    
        /* Every block in the extent is now garbage. */
        
        extent_manager->release_extent(entry->offset);
        
        switch (entry->state) {
            
            case gc_entry::state_active:
                fail("Marking something in the active extent as garbage.");
            
            /* Remove from the young extent queue */
            case gc_entry::state_young: {
                std::deque< gc_entry*, gnew_alloc<gc_entry*> >::iterator it;
                for (it = young_extent_queue.begin(); ; it++) {
                    assert(it != young_extent_queue.end());
                    if (*it == entry) {
                        young_extent_queue.erase(it);
                        break;
                    }
                }
                break;
            }
            
            /* Remove from the priority queue */
            case gc_entry::state_old:
                gc_pq.remove(entry->our_pq_entry);
                break;
            
            /* Notify the GC that the extent got released during GC */
            case gc_entry::state_in_gc:
                assert(gc_state.current_entry == entry);
                gc_state.current_entry = NULL;
                break;
        }
        
        if (entry->state != gc_entry::state_young) {
            gc_stats.unyoung_total_blocks -= extent_manager->extent_size / block_size;
        }
        
        delete entry;
        entries.set(extent_id, NULL);
        
    } else if (entry->state == gc_entry::state_old) {
    
        entry->our_pq_entry->update();
    }
}

void data_block_manager_t::start_reconstruct() {

    gc_state.step = gc_reconstruct;
}

// Marks the block at the given offset as alive, in the appropriate
// gc_entry in the entries table.  (This is used when we start up, when
// everything is presumed to be garbage, until we mark it as
// non-garbage.)
void data_block_manager_t::mark_live(off64_t offset) {
    assert(gc_state.step == gc_reconstruct);  // This is called at startup.

    unsigned int extent_id = (offset / extent_manager->extent_size);
    unsigned int block_id = (offset % extent_manager->extent_size) / block_size;

    if (entries.get(extent_id) == NULL) {
    
        gc_entry *entry = new gc_entry(extent_id * extent_manager->extent_size);
        entries.set(extent_id, entry);
        entry->our_pq_entry = gc_pq.push(entry);
        
        extent_manager->reserve_extent(entry->offset);
        
        // All are deemed garbage, and the extent is not young.
        gc_stats.unyoung_total_blocks += extent_manager->extent_size / BTREE_BLOCK_SIZE;
        gc_stats.unyoung_garbage_blocks += extent_manager->extent_size / BTREE_BLOCK_SIZE;
    }

    /* mark the block as alive */
    assert(entries.get(extent_id)->g_array[block_id] == 1);
    entries.get(extent_id)->g_array.set(block_id, 0);
    
    gc_stats.unyoung_garbage_blocks--;
}

void data_block_manager_t::end_reconstruct() {

    gc_state.step = gc_ready;
}

void data_block_manager_t::start_gc() {
    if (gc_state.step == gc_ready)
        run_gc();
}

/* TODO this currently cleans extent by extent, we should tune it to always have a certain number of outstanding blocks
 */
void data_block_manager_t::run_gc() {

    switch (gc_state.step) {
    
        case gc_ready:
            //TODO, need to make sure we don't gc the extent we're writing to
            if (gc_pq.empty() || !should_we_keep_gcing(*gc_pq.peak())) return;

            /* grab the entry */
            gc_state.current_entry = gc_pq.pop();
            gc_state.current_entry->our_pq_entry = NULL;
            gc_state.current_entry->state = gc_entry::state_in_gc;

            /* read all the live data into buffers */

            /* make sure the read callback knows who we are */
            gc_state.gc_read_callback.parent = this;

            for (unsigned int i = 0; i < extent_manager->extent_size / BTREE_BLOCK_SIZE; i++) {
                if (!gc_state.current_entry->g_array[i]) {
                    dbfile->read_async(
                        gc_state.current_entry->offset + (i * block_size),
                        block_size,
                        gc_state.gc_blocks + (i * block_size),
                        &(gc_state.gc_read_callback));
                    gc_state.refcount++;
                }
            }
            assert(gc_state.refcount > 0);
            gc_state.step = gc_read;
            break;
            
        case gc_read: {
            gc_state.refcount--;
            if (gc_state.refcount > 0) {
                /* We got a block, but there are still more to go */
                break;
            }    
            
            /* If other forces cause all of the blocks in the extent to become garbage
            before we even finish GCing it, they will set current_entry to NULL. */
            if (gc_state.current_entry == NULL) {
                gc_state.step = gc_ready;
                break;
            }
            
            /* an array to put our writes in */
            int num_writes = gc_state.current_entry->g_array.size() - gc_state.current_entry->g_array.count();
            log_serializer_t::write_t writes[num_writes];
            int current_write = 0;

            for (unsigned int i = 0; i < extent_manager->extent_size / BTREE_BLOCK_SIZE; i++) {
                /* We re-check the bit array here in case a write came in for one of the
                blocks we are GCing. We wouldn't want to overwrite the new valid data with
                out-of-date data. */
                if (!gc_state.current_entry->g_array[i]) {
                    writes[current_write].block_id = *((ser_block_id_t *) (gc_state.gc_blocks + (i * block_size)));
                    writes[current_write].buf = gc_state.gc_blocks + (i * block_size);
                    writes[current_write].callback = NULL;
                    current_write++;
                }
            }
            
            assert(current_write == num_writes);
            
            /* make sure the callback knows who we are */
            gc_state.gc_write_callback.parent = this;
            
            gc_state.step = gc_write;

            /* schedule the write */
            bool done = serializer->do_write(writes, num_writes, &gc_state.gc_write_callback);
            if (!done) break;
        }
            
        case gc_write:
            /* Our write should have forced all of the blocks in the extent to become garbage,
            which should have caused the extent to be released and gc_state.current_offset to
            become NULL. */
            assert(gc_state.current_entry == NULL);
            
            assert(gc_state.refcount == 0);

            gc_state.step = gc_ready;

            if(state == state_shutting_down) {
                actually_shutdown();
                return;
            }
            
            run_gc();   // We might want to start another GC round
            break;
            
        default: fail("Unknown gc_step");
    }
}

void data_block_manager_t::prepare_metablock(metablock_mixin_t *metablock) {

    assert(state == state_ready
           || (state == state_shutting_down && gc_state.step == gc_read));

    if (last_data_extent) {
        metablock->last_data_extent = last_data_extent->offset;
        metablock->blocks_in_last_data_extent =
            last_data_extent->g_array.size() - last_data_extent->g_array.count();
        
    } else {
        metablock->last_data_extent = NULL_OFFSET;
        metablock->blocks_in_last_data_extent = 0;
    }
}

bool data_block_manager_t::shutdown(shutdown_callback_t *cb) {
    assert(cb);
    assert(state == state_ready);
    state = state_shutting_down;

    if(gc_state.step != gc_ready) {
        shutdown_callback = cb;
        return false;
    } else {
        shutdown_callback = NULL;
        actually_shutdown();
        return true;
    }
}

void data_block_manager_t::actually_shutdown() {
    
    assert(state == state_shutting_down);
    state = state_shut_down;
    
    if (last_data_extent) {
        delete last_data_extent;
        last_data_extent = NULL;
    }
    
    while (!young_extent_queue.empty()) {
        delete young_extent_queue.front();
        young_extent_queue.pop_front();
    }
    
    while (!gc_pq.empty()) {
        delete gc_pq.pop();
    }
    
    if (shutdown_callback) shutdown_callback->on_datablock_manager_shutdown();
}

off64_t data_block_manager_t::gimme_a_new_offset() {
    
    if (!last_data_extent) {
    
        last_data_extent = new gc_entry(extent_manager);

        unsigned int extent_id = last_data_extent->offset / extent_manager->extent_size;
        assert(entries.get(extent_id) == NULL);
        entries.set(extent_id, last_data_extent);
    }
    
    assert(last_data_extent->state == gc_entry::state_active);
    
    unsigned blocks_in_last_data_extent = last_data_extent->g_array.size() - last_data_extent->g_array.count();
    assert(blocks_in_last_data_extent < extent_manager->extent_size / block_size);
    
    off64_t offset = last_data_extent->offset + blocks_in_last_data_extent * block_size;
    
    assert(last_data_extent->g_array[blocks_in_last_data_extent]);
    last_data_extent->g_array.set(blocks_in_last_data_extent, 0);
    
    if (last_data_extent->g_array.count() == 0) {
        
        last_data_extent->state = gc_entry::state_young;
        young_extent_queue.push_back(last_data_extent);
        mark_unyoung_entries();
        last_data_extent = NULL;
    }
    
    return offset;
}

// Looks at young_extent_queue and pops things off the queue that are
// no longer deemed young, putting them on the priority queue.
void data_block_manager_t::mark_unyoung_entries() {

    while (young_extent_queue.size() > GC_YOUNG_EXTENT_MAX_SIZE) {
        remove_last_unyoung_entry();
    }

    gc_entry::timestamp_t current_time = gc_entry::current_timestamp();

    while (!young_extent_queue.empty()
           && current_time - young_extent_queue.front()->timestamp > GC_YOUNG_EXTENT_TIMELIMIT_MICROS) {
        remove_last_unyoung_entry();
    }
}

// Pops young_extent_queue and puts it on the priority queue.
// Assumes young_extent_queue is not empty.
void data_block_manager_t::remove_last_unyoung_entry() {

    assert(!young_extent_queue.empty());

    gc_entry *entry = young_extent_queue.front();
    young_extent_queue.pop_front();
    
    assert(entry->state == gc_entry::state_young);
    entry->state = gc_entry::state_old;
    entry->our_pq_entry = gc_pq.push(entry);
    gc_stats.unyoung_total_blocks += extent_manager->extent_size / BTREE_BLOCK_SIZE;
    gc_stats.unyoung_garbage_blocks += entry->g_array.count();
}


/* functions for gc structures */

// Right now we start gc'ing when the garbage ratio is >= 0.75 (75%
// garbage) and we gc all blocks with that ratio or higher.


// Answers the following question: We're in the middle of gc'ing, and
// look, it's the next largest entry.  Should we keep gc'ing?  Returns
// false when the entry is active or young, or when its garbage ratio
// is lower than GC_THRESHOLD_RATIO_*.
bool data_block_manager_t::should_we_keep_gcing(const gc_entry& entry) const {
    return garbage_ratio() > cmd_config->gc_low_ratio;
}

// Answers the following question: Do we want to bother gc'ing?
// Returns true when our garbage_ratio is greater than
// GC_THRESHOLD_RATIO_*.
bool data_block_manager_t::do_we_want_to_start_gcing() const {
    return garbage_ratio() > cmd_config->gc_high_ratio;
}

/* !< is x less than y */
bool data_block_manager_t::Less::operator() (const data_block_manager_t::gc_entry *x, const data_block_manager_t::gc_entry *y) {
    return x->g_array.count() < y->g_array.count();
}

/****************
 *Stat functions*
 ****************/

// This will return NaN when gc_stats.unyoung_total_blocks is zero.
float data_block_manager_t::garbage_ratio() const {
    // TODO: not divide by zero?
    return (float) gc_stats.unyoung_garbage_blocks / (float) gc_stats.unyoung_total_blocks;
}
