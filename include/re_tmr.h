/**
 * @file re_tmr.h  Interface to timer implementation
 *
 * Copyright (C) 2010 Creytiv.com
 */


/**
 * Defines the timeout handler
 *
 * @param arg Handler argument
 */
typedef void (tmr_h)(void *arg);

/** Defines a timer */
struct tmr {
	struct le le;       /**< Linked list element */
	struct list *tmrl;  /**< Parent list         */
	tmr_h *th;          /**< Timeout handler     */
	void *arg;          /**< Handler argument    */
	uint64_t jfs;       /**< Jiffies for timeout */
};


void     tmr_poll(struct list *tmrl);
uint64_t tmr_jiffies(void);
uint64_t tmr_next_timeout(struct list *tmrl);
void     tmr_debug(void);

void     tmr_init(struct tmr *tmr);
void     tmr_start(struct tmr *tmr, uint64_t delay, tmr_h *th, void *arg);
void     tmr_cancel(struct tmr *tmr);
uint64_t tmr_get_expire(const struct tmr *tmr);