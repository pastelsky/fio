#ifndef FIO_DIR_BLOCKS_H
#define FIO_DIR_BLOCKS_H

#include "fio.h"

/**
 * Initialize block tracking for a target directory
 * @param td        Thread data
 * @return          0 on success, error code on failure
 */
int init_targ_dir_blocks(struct thread_data *td);

/**
 * Check if a given block belongs to the target directory
 * @param td        Thread data
 * @param f         File being accessed
 * @param block     Block number to check
 * @return          true if block belongs to target directory, false otherwise
 */
bool is_block_in_targ_dir(struct thread_data *td, struct fio_file *f, uint64_t block);

/**
 * Clean up resources used for tracking target directory blocks
 * @param td        Thread data
 */
void cleanup_targ_dir_blocks(struct thread_data *td);

#endif /* FIO_DIR_BLOCKS_H */