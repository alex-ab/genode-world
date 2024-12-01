/*
 * \brief  File handling
 * \author Alexander Boettcher
 * \date   2025-11-28
 */

/*
 * Copyright (C) 2025 Alexander Boettcher
 *
 * This file is part of Seoul, which is distributed
 * under the terms of the GNU General Public License version 2.
 */


#include <util/bit_allocator.h>
#include <file_system_session/connection.h>


namespace Seoul {
	class Filesystem;

	using namespace Genode;
	using namespace File_system;

	typedef File_system::Packet_descriptor Packet;
	typedef File_system::Session           Session;
}


class Seoul::Filesystem : public StaticReceiver<Filesystem>
{
	private:

		Env                     &env;
		Heap                     heap  { env.ram(), env.rm() };
		Allocator_avl            alloc { &heap };
		File_system::Connection  fs    { env, alloc };
		Dir_handle               root  { fs.dir("/", false) };
		Session::Tx::Source     &tx    { *fs.tx() };

		Genode::Mutex            mutex { };

		unsigned const           fs_id { 1 }; /* change if multiple instantiated */

		size_t const _packet_max { tx.bulk_buffer_size() / Session::TX_QUEUE_SIZE };

		Signal_handler<Filesystem> _handler { env.ep(), *this, &Filesystem::_handle_submit };

		void _handle_ack()
		{
			error(__func__);

			Genode::Mutex::Guard guard(mutex);

			while (tx.ack_avail()) {
				auto packet = tx.get_acked_packet();

#if 0
		Node_handle handle()    const { return _handle;   }
		Opcode      operation() const { return _op;       }
		seek_off_t  position()  const { return _op != Opcode::WRITE_TIMESTAMP ? _position : 0; }
		size_t      length()    const { return _op != Opcode::WRITE_TIMESTAMP ? _length : 0;   }
		bool        succeeded() const { return _success;  }
#endif

				switch (packet.operation()) {
				case Packet::READ_READY:
					error(__func__, " READ READY");
					break;

				case Packet::READ:
				{
					bool wakeup = false;

					error(__func__, " READ ", packet.succeeded() ? "succeeded" : "failure", " handle=", packet.handle(), " len=", packet.length(), "/?", sizeof(Directory_entry), " node handle ", packet.handle().value, " offset=", packet.offset());
					if (packet.succeeded()) {
						auto const count = packet.length() / sizeof(Directory_entry);
						auto entries = reinterpret_cast<Directory_entry *>(tx.packet_content(packet));
						for (unsigned i = 0; i < count; i++) {
							error("dir: ", (char const *)((entries + i)->name.buf));
						}
					}
					if (with_packet(packet, [&](auto) {
						error("success");
						if (!packet.succeeded() || !_queue_read_dir) {
							error("queue read dir failure");
							return;
						}

						MessageFs * r_msg = reinterpret_cast<MessageFs *>(_queue_read_dir);

						auto const count = packet.length() / sizeof(Directory_entry);
						auto entries = reinterpret_cast<Directory_entry *>(tx.packet_content(packet));

						unsigned start = 0;

#if 0
						error("start queue_reads=", _queue_reads, " ", r_msg->entry_skip);
						if (r_msg->entry_skip) {
							auto const diff    = min(r_msg->entry_skip, unsigned(count));
							_queue_reads      -= diff;
							r_msg->entry_skip -= diff;
							start = diff;
						}

						error("start queue_reads=", _queue_reads, " ", r_msg->entry_skip, " ", count);
#endif

						for (unsigned i = start; i < count; i++) {
							auto &entry = entries[i];
							auto  name = reinterpret_cast<char const *>(entry.name.buf);
							auto  name_len = strnlen(name, MAX_NAME_LEN);
							error("dir ", i, " : ", name, " ", name_len, " inode=", entry.inode);
							if (!r_msg->add_read_dir(name, unsigned(name_len), entry.inode,
							                         entry.type == Node_type::DIRECTORY,
							                         entry.type == Node_type::SYMLINK))
								error("queue read failure");
							else {
								error("queue_reads=", _queue_reads);
								if (_queue_reads) {
									_queue_reads --;
									if (!_queue_reads)
										wakeup = true;
								}
							}
						}
					})) {
						if (wakeup)
							_queue_read_block.wakeup();
						continue;
					}
					else
						error("unknown read");

					break;
				}
				case Packet::WRITE:
					error(__func__, " WRITE");
					break;

				case Packet::SYNC:
					error(__func__, " SYNC");
					break;

				case Packet::CONTENT_CHANGED:
					error(__func__, " CONTENT CHANGED");
					break;

				case Packet::WRITE_TIMESTAMP:
					error(__func__, " WRITE TIMESTAMP");
					break;
				};

				tx.release_packet(packet);
			}
		}

		void _handle_submit()
		{
			error(__func__);
			_handle_ack();
		}

		struct {
			Dir_handle  dir  { 0 };
			String<128> path { };
		} _nodeid_dirs[64] { };

		Bit_allocator<64> _idx_alloc   { }; 
		Packet            _packets[64] { };

		uintptr_t   _queue_read_dir   { };
		Blockade    _queue_read_block { };
		unsigned    _queue_reads      { };

		bool with_new_packet(auto const &fn)
		{
			return _idx_alloc.alloc().convert<bool>([&](auto const &id) {
				fn(id);
				return true;
			}, [](auto) { return false; });
		}

		bool with_packet(auto &packet, auto const &fn)
		{
			for (unsigned idx = 0; idx < 64; idx++) {
				auto &p = _packets[idx];

				if (p.offset() != packet.offset() || p.size() != packet.size())
					continue;

				fn(packet);

				_idx_alloc.free(idx);

				tx.release_packet(packet);

				return true;
			}

			return false;
		}

		void with_open_dir(MessageFs const &msg, auto &entry, auto const &fn)
		{
			if (msg.nodeid > 1 && !entry.dir.value) {
				Dir_handle h { fs.dir(entry.path.string(), false) };

				entry.dir = h;

				log(" File::open dir nodeid=", msg.nodeid, " -> fh=", h.value, " '", entry.path, "'");

				fn();
			} else
				fn();
		}

	public:

		Filesystem(Env &env, Motherboard &mb) : env(env)
		{
			if (root.value != 0) {
				error("Filesystem offline - unexpected state");
				return;
			}

			fs.sigh(_handler);

			mb.bus_fs.add(this, receive_static<MessageFs>);

			/* what about nodeid[0] ??? */
			auto &entry = _nodeid_dirs[1];

			entry.dir  = root;
			entry.path = "";
		}

		bool receive(MessageFs &msg)
		{
			if (fs_id != msg.id)
				return false;

			/* XXX dynamic lookup structure required */
			if (msg.nodeid >= sizeof(_nodeid_dirs) / sizeof(_nodeid_dirs[0])) {
				error("nodeid=", msg.nodeid, "out of range - "
				      "msg.type=", unsigned(msg.type));
				msg.fail();
				return true;
			}

			bool wakeup = false;

			switch (msg.type) {
			case MessageFs::OPEN_DIR:
			{
				Genode::Mutex::Guard guard(mutex);

				log(" File::OPEN_DIR nodeid=", msg.nodeid);

				auto &entry = _nodeid_dirs[msg.nodeid];

				try {
					with_open_dir(msg, entry, [&]() {
						log (" File::OPEN_DIR nodeid=", msg.nodeid, " -> fh=", entry.dir.value, " '", entry.path, "'");

						msg.fh = entry.dir.value;
					});
				} catch (...) {
					error("open dir failed '", entry.path, "'");
					msg.fail();
				}
				break;
			}
			case MessageFs::CLOSE_DIR:
			{
				if (msg.fh == root.value) /* keep root dir open */
					break;

				Genode::Mutex::Guard guard(mutex);

				auto &entry = _nodeid_dirs[msg.nodeid];

				if (entry.dir.value != msg.fh)
					warning("close_dir: fh does not match inode");

				Dir_handle h { msg.fh };

				try {
					fs.close(h);
					entry.dir.value = 0;
				} catch (...) {
					error("closing dir failed");
				}
				break;
			}
			case MessageFs::READ_DIR:
			{
				{
					Genode::Mutex::Guard guard(mutex);

					log(" File::READ_DIR fh=", msg.fh);

					if (_nodeid_dirs[msg.nodeid].dir.value != msg.fh)
						warning("read_dir: fh does not match inode");

					if (_queue_read_dir) {
						error("unexpected read dir state");
						return false;
					}

					Dir_handle const dir { msg.fh };

					_queue_reads = fs.num_entries(dir);

					if (!_queue_reads)
						break;

					_queue_read_dir = uintptr_t(&msg);

					queue_read_dir(dir, _queue_reads);
				}

				tx.wakeup();

				_queue_read_block.block();

				{
					Genode::Mutex::Guard guard(mutex);

					_queue_read_dir = { };
				}

				break;
			}
			case MessageFs::GET_ATTR:
			{
				try {
					auto &entry = _nodeid_dirs[msg.nodeid];

					with_open_dir(msg, entry, [&]() {
						Status status = fs.status(entry.dir);

						msg.add_status(status.inode, status.size,
						               status.modification_time.ms_since_1970,
						               status.directory(), status.symlink());

						msg.writeable  = status.rwx.writeable;
						msg.readable   = status.rwx.readable;
						msg.executable = status.rwx.executable;

					});
				} catch(...) {
					msg.fail();
					error(" File::GET_ATTR exception");
				}
				break;
			}
			case MessageFs::LOOKUP:
			{
				auto * path = reinterpret_cast<char const *>(msg.buffer.start);

				try {
					auto &parent_dir = _nodeid_dirs[msg.nodeid];

					with_open_dir(msg, parent_dir, [&]() {

						Dir_handle parent = parent_dir.dir;

						log(" File::LOOKUP parent inode=", msg.nodeid," fh=", parent.value, " path='", path, "'");

						/* STAT_ONLY leads to already_open exception for files XXX */
						File_handle h = fs.file(parent, path, READ_ONLY, false);

						Status status = fs.status(h);

						log(" File::LOOKUP -> inode=", status.inode, " size=", status.size, " time=", status.modification_time.ms_since_1970);

						if (status.directory()) {

							String<128> g_path { parent_dir.path, "/", path };

							log(" File::LOOKUP - dir '", g_path, "' -> nodeid=", status.inode);

							if (status.inode >= sizeof(_nodeid_dirs) / sizeof(_nodeid_dirs[0])) {
								error(" File::LOOKUP - inode too large ", status.inode);
								return;
							}

							auto &dir = _nodeid_dirs[status.inode];

							if (dir.dir.value)
								error("already open ? dir nodeid=", status.inode, " already fh=", dir.dir.value, " '", dir.path, "' vs '", g_path, "'");

							dir.path = g_path;
						}

						msg.add_status(status.inode, status.size,
						               status.modification_time.ms_since_1970,
						               status.directory(), status.symlink());

						msg.writeable  = status.rwx.writeable;
						msg.readable   = status.rwx.readable;
						msg.executable = status.rwx.executable;

						fs.close(h);
					});
				} catch (...) {
					msg.fail();
					error("exception ", __LINE__);
				}
				break;
			}
			default:
				Logging::panic("unsupported call");
				break;
			}

			if (wakeup)
				tx.wakeup();

			return true;
		}

		void queue_read_dir(Dir_handle const &dir_handle, unsigned &count)
		{
			auto num_entries = count;

			/* XXX what if too many entries for buffer */
			auto packet_size = sizeof(Directory_entry);

			/* XXX alloc packet failures */
			for (unsigned i = 0; i < num_entries; i++) {

				with_new_packet([&](auto &idx) {

					auto &packet = _packets[idx];

					packet = Packet(tx.alloc_packet(packet_size),
					                dir_handle, Packet::READ,
					                packet_size, i * packet_size);

					if (!tx.ready_to_submit())
						error("read_dir: not ready to submit");

					if (!tx.try_submit_packet(packet))
						error("read_dir: could not submit");
				});
			}
		}
};
