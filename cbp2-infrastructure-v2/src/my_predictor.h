// my_predictor.h
// This file contains a sample gshare_predictor class, a Pentium M hybrid
// outcome predictor (pm_predictor), and a complete Pentium M outcome + target
// predictor (cpm_predictor).

#include <cstring>

// Fallback flag definitions in case branch.h only defines a subset
#ifndef BR_CONDITIONAL
#define BR_CONDITIONAL 1
#endif
#ifndef BR_INDIRECT
#define BR_INDIRECT 2
#endif
#ifndef BR_CALL
#define BR_CALL 4
#endif
#ifndef BR_RETURN
#define BR_RETURN 8
#endif

// ============================================================================
// 1. Sample gshare_predictor
// ============================================================================

class gshare_update : public branch_update {
public:
    unsigned int index;
};

class gshare_predictor : public branch_predictor {
public:
#define HISTORY_LENGTH  15
#define TABLE_BITS  15
    gshare_update u;
    branch_info bi;
    unsigned int history;
    unsigned char tab[1<<TABLE_BITS];

    gshare_predictor (void) : history(0) { 
        memset (tab, 0, sizeof (tab));
    }

    branch_update *predict (branch_info & b) {
        bi = b;
        if (b.br_flags & BR_CONDITIONAL) {
            u.index = 
                  (history << (TABLE_BITS - HISTORY_LENGTH)) 
                ^ (b.address & ((1<<TABLE_BITS)-1));
            u.direction_prediction (tab[u.index] >> 1);
        } else {
            u.direction_prediction (true);
        }
        u.target_prediction (0);
        return &u;
    }

    void update (branch_update *u, bool taken, unsigned int target) {
        if (bi.br_flags & BR_CONDITIONAL) {
            unsigned char *c = &tab[((gshare_update*)u)->index];
            if (taken) {
                if (*c < 3) (*c)++;
            } else {
                if (*c > 0) (*c)--;
            }
            history <<= 1;
            history |= taken;
            history &= (1<<HISTORY_LENGTH)-1;
        }
    }
};

// ============================================================================
// 2. Pentium M Hybrid Branch Predictor (Bimodal + Global Outcome Predictor)
// ============================================================================

class pm_update : public branch_update {
public:
    unsigned int index;        // Bimodal table index
    unsigned int global_set;   // Global predictor set index
    unsigned int global_tag;   // Global predictor tag
    int global_way;            // Matching way in global predictor (-1 if miss)
    bool bimodal_pred;         // Saved bimodal direction prediction
};

class pm_predictor : public branch_predictor {
public:
    static const int BIMODAL_BITS = 12;               // 4,096 entries
    static const int BIMODAL_SIZE = 1 << BIMODAL_BITS;
    static const int GLOBAL_SETS  = 512;              // 512 sets * 4 ways = 2,048 entries
    static const int GLOBAL_WAYS  = 4;
    static const int PIR_BITS     = 15;               // 15-bit Path Information Register
    static const int PIR_MASK     = (1 << PIR_BITS) - 1;

    struct global_entry {
        bool valid;
        unsigned char tag;     // 6-bit tag
        unsigned char ctr;     // 2-bit saturating counter (0..3)
        unsigned char lru;     // LRU age (0 = MRU, 3 = LRU)
    };

    pm_update u;
    branch_info bi;
    unsigned int pir;
    unsigned char bimodal[BIMODAL_SIZE];
    global_entry global_tab[GLOBAL_SETS][GLOBAL_WAYS];

    pm_predictor (void) : pir(0) {
        // Initialize bimodal counters to Weakly Not Taken (1)
        memset (bimodal, 1, sizeof (bimodal));
        memset (global_tab, 0, sizeof (global_tab));
        for (int s = 0; s < GLOBAL_SETS; s++) {
            for (int w = 0; w < GLOBAL_WAYS; w++) {
                global_tab[s][w].lru = w;
            }
        }
    }

    // Touch a way in a 4-way set to mark it Most Recently Used (age 0)
    void touch_global_lru (unsigned int set, int way) {
        unsigned char old_age = global_tab[set][way].lru;
        for (int w = 0; w < GLOBAL_WAYS; w++) {
            if (w != way && global_tab[set][w].lru < old_age) {
                global_tab[set][w].lru++;
            }
        }
        global_tab[set][way].lru = 0;
    }

    branch_update *predict (branch_info & b) {
        bi = b;

        if (b.br_flags & BR_CONDITIONAL) {
            // 1. Bimodal lookup (indexed by lower 12 bits of PC)
            u.index = b.address & (BIMODAL_SIZE - 1);
            u.bimodal_pred = (bimodal[u.index] >> 1) != 0;

            // 2. Global lookup (9-bit set index and 6-bit tag hashed from PC and PIR)
            u.global_set = ((b.address >> 4) ^ (pir & 0x1FF)) & (GLOBAL_SETS - 1);
            u.global_tag = ((b.address >> 13) ^ (pir >> 9)) & 0x3F;
            u.global_way = -1;

            for (int w = 0; w < GLOBAL_WAYS; w++) {
                if (global_tab[u.global_set][w].valid &&
                    global_tab[u.global_set][w].tag == u.global_tag) {
                    u.global_way = w;
                    break;
                }
            }

            // 3. Hybrid selection: Global hit overrides Bimodal
            if (u.global_way != -1) {
                bool global_pred = (global_tab[u.global_set][u.global_way].ctr >> 1) != 0;
                u.direction_prediction (global_pred);
            } else {
                u.direction_prediction (u.bimodal_pred);
            }
        } else {
            u.direction_prediction (true);
        }

        // Target prediction is not performed in pm_predictor
        u.target_prediction (0);
        return &u;
    }

    void update (branch_update *u_ptr, bool taken, unsigned int target) {
        pm_update *pu = (pm_update *)u_ptr;

        if (bi.br_flags & BR_CONDITIONAL) {
            if (pu->global_way != -1) {
                // Global Hit: update the matching global counter and LRU
                unsigned char *gc = &global_tab[pu->global_set][pu->global_way].ctr;
                if (taken) {
                    if (*gc < 3) (*gc)++;
                } else {
                    if (*gc > 0) (*gc)--;
                }
                touch_global_lru (pu->global_set, pu->global_way);
            } else {
                // Global Miss: update the bimodal counter
                unsigned char *bc = &bimodal[pu->index];
                if (taken) {
                    if (*bc < 3) (*bc)++;
                } else {
                    if (*bc > 0) (*bc)--;
                }

                // Allocate a new entry in the global table if bimodal mispredicted
                if (pu->bimodal_pred != taken) {
                    int victim = 0;
                    for (int w = 0; w < GLOBAL_WAYS; w++) {
                        if (!global_tab[pu->global_set][w].valid) {
                            victim = w;
                            break;
                        }
                        if (global_tab[pu->global_set][w].lru == GLOBAL_WAYS - 1) {
                            victim = w;
                        }
                    }
                    global_tab[pu->global_set][victim].valid = true;
                    global_tab[pu->global_set][victim].tag   = pu->global_tag;
                    global_tab[pu->global_set][victim].ctr   = taken ? 2 : 1; // Weakly Taken / Not Taken
                    touch_global_lru (pu->global_set, victim);
                }
            }

            // Update 15-bit Path Information Register (PIR)
            pir = (((pir << 2) ^ ((bi.address >> 4) & 0x1FFF)) | (taken ? 1 : 0)) & PIR_MASK;
        }
    }
};

// ============================================================================
// 3. Complete Pentium M Branch Predictor (Extra Credit)
//    Outcome (Bimodal + Global + Loop) + Target (BTB + iBTB + RAS)
// ============================================================================

class cpm_update : public branch_update {
public:
    unsigned int index;        // Bimodal index
    unsigned int global_set;   // Global predictor set
    unsigned int global_tag;   // Global predictor tag
    int global_way;            // Matching global way (-1 if miss)
    unsigned int loop_set;     // Loop predictor set
    unsigned int loop_tag;     // Loop predictor tag
    int loop_way;              // Matching loop way (-1 if miss)
    unsigned int btb_set;      // BTB set
    unsigned int btb_tag;      // BTB tag
    int btb_way;               // Matching BTB way (-1 if miss)
    unsigned int ibtb_index;   // Indirect BTB index
    unsigned int ibtb_tag;     // Indirect BTB tag
    bool bimodal_pred;         // Saved bimodal prediction
};

class cpm_predictor : public branch_predictor {
public:
    // Outcome table parameters
    static const int BIMODAL_BITS = 12;
    static const int BIMODAL_SIZE = 1 << BIMODAL_BITS;
    static const int GLOBAL_SETS  = 512;
    static const int GLOBAL_WAYS  = 4;
    static const int LOOP_SETS    = 64;   // 64 sets * 2 ways = 128 loop entries
    static const int LOOP_WAYS    = 2;
    static const int PIR_BITS     = 15;
    static const int PIR_MASK     = (1 << PIR_BITS) - 1;

    // Target table parameters
    static const int BTB_SETS     = 512;  // 512 sets * 4 ways = 2,048 BTB entries
    static const int BTB_WAYS     = 4;
    static const int IBTB_SIZE    = 256;  // 256-entry Indirect BTB
    static const int RAS_SIZE     = 16;   // 16-entry Return Address Stack

    struct global_entry {
        bool valid;
        unsigned char tag;
        unsigned char ctr;
        unsigned char lru;
    };

    struct loop_entry {
        bool valid;
        unsigned char tag;         // 6-bit tag (PC[15:10])
        unsigned short limit;      // Learned loop iteration count
        unsigned short current;    // Current iteration counter
        unsigned char confidence;  // 2-bit confidence counter (0..3)
        bool dir;                  // Loop body branch direction
        unsigned char lru;
    };

    struct btb_entry {
        bool valid;
        unsigned int tag;
        unsigned int target;
        unsigned char lru;
    };

    struct ibtb_entry {
        bool valid;
        unsigned char tag;
        unsigned int target;
    };

    cpm_update u;
    branch_info bi;
    unsigned int pir;

    unsigned char bimodal[BIMODAL_SIZE];
    global_entry global_tab[GLOBAL_SETS][GLOBAL_WAYS];
    loop_entry loop_tab[LOOP_SETS][LOOP_WAYS];
    btb_entry btb[BTB_SETS][BTB_WAYS];
    ibtb_entry ibtb[IBTB_SIZE];
    unsigned int ras[RAS_SIZE];
    int ras_top;

    cpm_predictor (void) : pir(0), ras_top(0) {
        memset (bimodal, 1, sizeof (bimodal));
        memset (global_tab, 0, sizeof (global_tab));
        memset (loop_tab, 0, sizeof (loop_tab));
        memset (btb, 0, sizeof (btb));
        memset (ibtb, 0, sizeof (ibtb));
        memset (ras, 0, sizeof (ras));

        for (int s = 0; s < GLOBAL_SETS; s++)
            for (int w = 0; w < GLOBAL_WAYS; w++)
                global_tab[s][w].lru = w;

        for (int s = 0; s < BTB_SETS; s++)
            for (int w = 0; w < BTB_WAYS; w++)
                btb[s][w].lru = w;
    }

    void touch_global_lru (unsigned int set, int way) {
        unsigned char old_age = global_tab[set][way].lru;
        for (int w = 0; w < GLOBAL_WAYS; w++) {
            if (w != way && global_tab[set][w].lru < old_age)
                global_tab[set][w].lru++;
        }
        global_tab[set][way].lru = 0;
    }

    void touch_btb_lru (unsigned int set, int way) {
        unsigned char old_age = btb[set][way].lru;
        for (int w = 0; w < BTB_WAYS; w++) {
            if (w != way && btb[set][w].lru < old_age)
                btb[set][w].lru++;
        }
        btb[set][way].lru = 0;
    }

    branch_update *predict (branch_info & b) {
        bi = b;
        bool dir_pred = true;

        // --------------------------------------------------------------------
        // 1. Branch Outcome Prediction (Bimodal + Global + Loop)
        // --------------------------------------------------------------------
        if (b.br_flags & BR_CONDITIONAL) {
            // Bimodal lookup
            u.index = b.address & (BIMODAL_SIZE - 1);
            u.bimodal_pred = (bimodal[u.index] >> 1) != 0;
            dir_pred = u.bimodal_pred;

            // Global lookup
            u.global_set = ((b.address >> 4) ^ (pir & 0x1FF)) & (GLOBAL_SETS - 1);
            u.global_tag = ((b.address >> 13) ^ (pir >> 9)) & 0x3F;
            u.global_way = -1;

            for (int w = 0; w < GLOBAL_WAYS; w++) {
                if (global_tab[u.global_set][w].valid &&
                    global_tab[u.global_set][w].tag == u.global_tag) {
                    u.global_way = w;
                    dir_pred = (global_tab[u.global_set][w].ctr >> 1) != 0;
                    break;
                }
            }

            // Loop predictor lookup: indexed by PC[9:4], tagged by PC[15:10]
            u.loop_set = (b.address >> 4) & (LOOP_SETS - 1);
            u.loop_tag = (b.address >> 10) & 0x3F;
            u.loop_way = -1;

            for (int w = 0; w < LOOP_WAYS; w++) {
                if (loop_tab[u.loop_set][w].valid &&
                    loop_tab[u.loop_set][w].tag == u.loop_tag) {
                    u.loop_way = w;
                    // Override global/bimodal if loop entry has maximum confidence
                    if (loop_tab[u.loop_set][w].confidence == 3) {
                        if (loop_tab[u.loop_set][w].current == loop_tab[u.loop_set][w].limit) {
                            dir_pred = !loop_tab[u.loop_set][w].dir;
                        } else {
                            dir_pred = loop_tab[u.loop_set][w].dir;
                        }
                    }
                    break;
                }
            }
        }
        u.direction_prediction (dir_pred);

        // --------------------------------------------------------------------
        // 2. Branch Target Prediction (BTB + iBTB + RAS)
        // --------------------------------------------------------------------
        unsigned int target_pred = 0;

        // Direct BTB lookup: indexed by PC[12:4] (9 bits)
        u.btb_set = (b.address >> 4) & (BTB_SETS - 1);
        u.btb_tag = b.address >> 13;
        u.btb_way = -1;

        for (int w = 0; w < BTB_WAYS; w++) {
            if (btb[u.btb_set][w].valid && btb[u.btb_set][w].tag == u.btb_tag) {
                u.btb_way = w;
                target_pred = btb[u.btb_set][w].target;
                break;
            }
        }

        // Indirect BTB lookup: indexed and tagged by PIR XOR PC
        u.ibtb_index = (((pir >> 5) & 0xFF) ^ ((b.address >> 4) & 0xFF)) & (IBTB_SIZE - 1);
        u.ibtb_tag   = ((pir & 0xFF) ^ ((b.address >> 10) & 0xFF)) & 0xFF;

        if ((b.br_flags & BR_RETURN) && ras_top > 0) {
            target_pred = ras[(ras_top - 1) & (RAS_SIZE - 1)];
        } else if (b.br_flags & BR_INDIRECT) {
            if (ibtb[u.ibtb_index].valid && ibtb[u.ibtb_index].tag == u.ibtb_tag) {
                target_pred = ibtb[u.ibtb_index].target;
            }
        }

        u.target_prediction (target_pred);
        return &u;
    }

    void update (branch_update *u_ptr, bool taken, unsigned int target) {
        cpm_update *cu = (cpm_update *)u_ptr;

        // --------------------------------------------------------------------
        // 1. Update Outcome Predictors (Loop, Global, Bimodal)
        // --------------------------------------------------------------------
        if (bi.br_flags & BR_CONDITIONAL) {
            // Update Loop Predictor
            if (cu->loop_way != -1) {
                loop_entry &le = loop_tab[cu->loop_set][cu->loop_way];
                if (taken == le.dir) {
                    le.current++;
                } else {
                    if (le.current == le.limit && le.limit > 0) {
                        if (le.confidence < 3) le.confidence++;
                    } else {
                        le.limit = le.current;
                        le.confidence = 1;
                    }
                    le.current = 0;
                }
                le.lru = 0;
                loop_tab[cu->loop_set][1 - cu->loop_way].lru = 1;
            } else {
                // Allocate in loop predictor on replacement
                int victim = (loop_tab[cu->loop_set][0].lru >= loop_tab[cu->loop_set][1].lru) ? 0 : 1;
                loop_entry &le = loop_tab[cu->loop_set][victim];
                le.valid      = true;
                le.tag        = cu->loop_tag;
                le.dir        = taken;
                le.current    = 1;
                le.limit      = 0;
                le.confidence = 0;
                le.lru        = 0;
                loop_tab[cu->loop_set][1 - victim].lru = 1;
            }

            // Update Global and Bimodal Predictors
            if (cu->global_way != -1) {
                unsigned char *gc = &global_tab[cu->global_set][cu->global_way].ctr;
                if (taken) {
                    if (*gc < 3) (*gc)++;
                } else {
                    if (*gc > 0) (*gc)--;
                }
                touch_global_lru (cu->global_set, cu->global_way);
            } else {
                unsigned char *bc = &bimodal[cu->index];
                if (taken) {
                    if (*bc < 3) (*bc)++;
                } else {
                    if (*bc > 0) (*bc)--;
                }

                if (cu->bimodal_pred != taken) {
                    int victim = 0;
                    for (int w = 0; w < GLOBAL_WAYS; w++) {
                        if (!global_tab[cu->global_set][w].valid) {
                            victim = w;
                            break;
                        }
                        if (global_tab[cu->global_set][w].lru == GLOBAL_WAYS - 1) {
                            victim = w;
                        }
                    }
                    global_tab[cu->global_set][victim].valid = true;
                    global_tab[cu->global_set][victim].tag   = cu->global_tag;
                    global_tab[cu->global_set][victim].ctr   = taken ? 2 : 1;
                    touch_global_lru (cu->global_set, victim);
                }
            }
        }

        // --------------------------------------------------------------------
        // 2. Update Target Predictors (RAS, BTB, iBTB)
        // --------------------------------------------------------------------
        if (bi.br_flags & BR_RETURN) {
            if (ras_top > 0) ras_top--;
        } else if (bi.br_flags & BR_CALL) {
            ras[ras_top & (RAS_SIZE - 1)] = bi.address + 4;
            ras_top++;
        }

        if (taken) {
            if (bi.br_flags & BR_INDIRECT) {
                ibtb[cu->ibtb_index].valid  = true;
                ibtb[cu->ibtb_index].tag    = cu->ibtb_tag;
                ibtb[cu->ibtb_index].target = target;
            }

            if (cu->btb_way != -1) {
                btb[cu->btb_set][cu->btb_way].target = target;
                touch_btb_lru (cu->btb_set, cu->btb_way);
            } else {
                int victim = 0;
                for (int w = 0; w < BTB_WAYS; w++) {
                    if (!btb[cu->btb_set][w].valid) {
                        victim = w;
                        break;
                    }
                    if (btb[cu->btb_set][w].lru == BTB_WAYS - 1) {
                        victim = w;
                    }
                }
                btb[cu->btb_set][victim].valid  = true;
                btb[cu->btb_set][victim].tag    = cu->btb_tag;
                btb[cu->btb_set][victim].target = target;
                touch_btb_lru (cu->btb_set, victim);
            }
        }

        // --------------------------------------------------------------------
        // 3. Update PIR (Path Information Register)
        // --------------------------------------------------------------------
        if ((bi.br_flags & BR_CONDITIONAL) || taken) {
            pir = (((pir << 2) ^ ((bi.address >> 4) & 0x1FFF)) | (taken ? 1 : 0)) & PIR_MASK;
        }
    }
};