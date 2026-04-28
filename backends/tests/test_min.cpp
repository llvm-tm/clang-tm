/**
 * Minimal TinySTM test
 */

#include <stdio.h>
extern "C" {
#include "../TinySTM/include/stm.h"
}

int main() {
    printf("Init STM\n");
    stm_init();
    
    printf("Init thread\n");
    stm_init_thread();
    
    printf("Start tx\n");
    sigjmp_buf *env = stm_start((stm_tx_attr_t){0});
    
    if (env != NULL && sigsetjmp(*env, 0) == 0) {
        printf("Load counter\n");
        volatile int x = 0;
        stm_word_t val = stm_load((volatile stm_word_t*)&x);
        printf("Got val: %lu\n", (unsigned long)val);
        stm_store((volatile stm_word_t*)&x, val + 1);
        
        printf("Commit\n");
        if (stm_commit()) {
            printf("SUCCESS\n");
        } else {
            printf("FAILED\n");
        }
    }
    
    printf("Exit thread\n");
    stm_exit_thread();
    
    printf("Exit STM\n");
    stm_exit();
    return 0;
}