/**
 * @author Aleksandar Dragojevic aleksandar.dragojevic@epfl.ch
 *
 */

#ifndef WLPDSTM_CM_H_
#define WLPDSTM_CM_H_

#include "constants.h"

namespace wlpdstm {

	class Timid {
		public:
			static void GlobalInit() {
				// do nothing
			}

			void ThreadInit(unsigned tid) {
				// do nothing
			}
		
			void TxStart() {
				// do nothing
			}

			void TxCommit() {
				// do nothing
			}

			void TxAbort() {
				// do nothing
			}

			void TxRestart() {
				// do nothing
			}

			bool ShouldRestartWriteWrite(Timid *other) {
				return true;
			}

			bool ShouldRestartReadWrite(Timid *other) {
				return true;
			}

			bool ShouldRestartWriteRead(Timid *other) {
				return true;
			}

			bool ShouldRestart() const {
				return false;
			}
	};

	class Greedy {
	public:
		static void GlobalInit() {
			// do nothing
		}
		
		void ThreadInit(unsigned tid) {
			// do nothing
		}
		
		void TxStart() {
			if(!aborted) {
				this->current_ts = this->counter++;
			}

			this->aborted = false;
		}
		
		void TxCommit() {
			// do nothing
		}

		void TxAbort() {
			// do nothing
		}
		
		void TxRestart() {
			// do nothing
		}
		
		bool ShouldRestartWriteWrite(Greedy *other) {
			return shouldRestartConflict(other);
		}
		
		bool ShouldRestartReadWrite(Greedy *other) {
			//return shouldRestartConflict(other);
			// give priority to writers
			return true;
		}

		bool ShouldRestartWriteRead(Greedy *other) {
			other->aborted = true;
			return this->aborted;
		}
		
		bool ShouldRestart() const {
			return aborted;
		}

	private:
		bool shouldRestartConflict(Greedy *other) {
			if(this->aborted) {
				return true;
			}

			// synchronize counters
			// races here shouldn't really matter
			if(other->counter > this->counter) {
				this->counter = other->counter;
			} else if(other->counter < this->counter) {
				other->counter = this->counter;
			}

			// decide who to abort
			if(other->current_ts < this->current_ts) {
				this->aborted = true;
			} else if(other->current_ts > this->current_ts) {
				other->aborted = true;
			} else {
				if(other->tid < this->tid) {
					this->aborted = true;
				} else {
					other->aborted = true;
				}
			}

			return this->aborted;
		}
		
	private:
		union {
			struct {
				// was I aborted
				volatile bool aborted;

				unsigned tid;

				// local counter
				volatile uint64_t counter;

				volatile uint64_t current_ts;
			};
			
			char padding[CACHE_LINE_SIZE_BYTES];
		};
	};
	
}


namespace wlpdstm {

#ifdef CM_TIMID
	typedef Timid ContentionManager;
#elif defined CM_GREEDY
	typedef Greedy ContentionManager;
#endif /* cm class */
}

#endif /* WLPDSTM_CM_H_ */
