/*
 * \brief  Utilities for implementing VMMs on Genode/NOVA
 * \author Norman Feske
 * \date   2013-08-20
 */

/*
 * Copyright (C) 2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__VMM__VCPU_DISPATCHER_H_
#define _INCLUDE__VMM__VCPU_DISPATCHER_H_

namespace Vmm {

	using namespace Genode;

	class Vcpu_dispatcher;
}


/**
 * Thread that handles virtualization events of a 'Vmm::Vcpu_thread'
 */
class Vmm::Vcpu_dispatcher : public Genode::Thread_base
{
	private:

		Cap_connection &_cap;

		/**
		 * Portal entry point entered on virtualization events
		 *
		 * For each event type used as argument of the 'register_handler'
		 * function template, the compiler automatically generates a separate
		 * instance of this function. The sole purpose of this function is to
		 * call the 'Vcpu' member function corresponding to the event type.
		 */
		template <unsigned EV, typename DISPATCHER, void (DISPATCHER::*FUNC)()>
		static void _portal_entry()
		{
			/* obtain this pointer of the event handler */
			Genode::Thread_base *myself = Genode::Thread_base::myself();
			DISPATCHER *vd = static_cast<DISPATCHER *>(myself);

			/* call event-specific handler function */
			(vd->*FUNC)();

			/* continue execution of the guest */
			Nova::reply(myself->stack_top());
		}

	public:

		Vcpu_dispatcher(size_t stack_size, Cap_connection &cap)
		:
			Thread_base("vCPU dispatcher", stack_size),
			_cap(cap)
		{
			using namespace Genode;

			/* request creation of a 'local' EC */                              
			_tid.ec_sel = Native_thread::INVALID_INDEX - 1;                     
			Thread_base::start();                                               

		}

		/**
		 * Register virtualization event handler
		 */
		template <unsigned EV, typename DISPATCHER, void (DISPATCHER::*FUNC)()>
		bool register_handler(addr_t exc_base, Nova::Mtd mtd)
		{
			/*
			 * Let the compiler generate an instance of a portal entry
			 */
			void (*entry)() = &_portal_entry<EV, DISPATCHER, FUNC>;

			/* Create the portal at the desired selector index */
			_cap.rcv_window(exc_base + EV);

			Native_capability thread_cap(tid().ec_sel);
			Native_capability handler =
				_cap.alloc(thread_cap, (Nova::mword_t)entry, mtd.value());

			return handler.valid() && (exc_base + EV == handler.local_name());
		}

		/**
		 * Unused member of the 'Thread_base' interface
		 *
		 * Similarly to how 'Rpc_entrypoints' are handled, a 'Vcpu_dispatcher'
		 * comes with a custom initialization procedure, which does not call
		 * the thread's normal entry function. Instead, the thread's EC gets
		 * associated with several portals, each for handling a specific
		 * virtualization event.
		 */
		void entry() { }

		/**
		 * Return capability selector of the VCPU's SM and EC
		 *
		 * The returned number corresponds to the VCPU's semaphore selector.
		 * The consecutive number corresponds to the EC. The number returned by
		 * this function is used by the VMM code as a unique identifier of the
		 * VCPU. I.e., it gets passed as arguments for 'MessageHostOp'
		 * operations.
		 */
		Nova::mword_t sel_sm_ec()
		{
			return tid().exc_pt_sel + Nova::SM_SEL_EC;
		}
};

#endif /* _INCLUDE__VMM__VCPU_DISPATCHER_H_ */
