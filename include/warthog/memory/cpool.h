#ifndef WARTHOG_MEMORY_CPOOL_H
#define WARTHOG_MEMORY_CPOOL_H

// cpool.h
//
// A pool of pre-allocated memory specialised for the construction of
// single structs of a fixed size.
// To achieve efficient re-allocation each pre-allocated
// chunk of memory has associated with it a stack of memory offsets
// which have been previously freed.
// This introduces a 12.5% overhead to total memory consumption.
//
// @author: dharabor
// @created: 23/08/2012
//

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace warthog::memory
{

const uint32_t DEFAULT_CHUNK_SIZE = 1024 * 1024; // 1MB

class cchunk
{
public:
	cchunk(size_t obj_size, size_t pool_size)
	    : obj_size_(obj_size),
	      pool_size_(pool_size - (pool_size % obj_size)) // round down
	{
		if(pool_size_ < obj_size_)
		{
			std::cerr << "cchunk object size < pool size; "
			          << "setting pool size to object size" << std::endl;
			pool_size_ = obj_size_;
		}

		mem_  = new char[pool_size_];
		next_ = mem_;
		max_  = mem_ + pool_size_;

		freed_stack_ = new size_t[(pool_size_ / obj_size)];
		stack_size_  = 0;
	}

	~cchunk()
	{
		delete[] mem_;
		delete[] freed_stack_;
	}

	inline void
	reclaim()
	{
		next_       = mem_;
		stack_size_ = 0;
	}

	inline char*
	allocate()
	{
		if(next_ < max_)
		{
			char* retval = next_;
			next_       += obj_size_;
			return retval;
		}

		if(stack_size_ > 0)
		{
			--stack_size_;
			return mem_ + freed_stack_[stack_size_];
		}

		return 0;
	}

	inline void
	deallocate(char* addr)
	{
#ifndef NDEBUG
		assert(mem_ >= addr);
		if((size_t)(addr - mem_) >= pool_size_)
		{
			std::cerr << "err; cchunk; freeing memory outside"
			             " range of the chunk at addr: "
			          << &mem_ << "\n";
		}
#endif

		freed_stack_[stack_size_] = (size_t)(addr - mem_);
		stack_size_++;
	}

	inline bool
	contains(char* addr)
	{
#ifndef NDEBUG
		assert(mem_ >= addr);
#endif
		if((unsigned)(addr - mem_) < pool_size_) { return true; }
		return false;
	}

	inline char*
	first_addr()
	{
		return mem_;
	}

	inline size_t
	pool_size()
	{
		return pool_size_;
	}

	inline size_t
	mem()
	{
		size_t bytes = sizeof(*this);
		bytes       += sizeof(char) * pool_size_;
		bytes       += sizeof(int) * (pool_size_ / obj_size_);
		return bytes;
	}

	void
	print(std::ostream& out)
	{
		out << "cchunk pool_size: " << pool_size_ << " obj_size: " << obj_size_
		    << " freed_stack_ size: " << stack_size_;
	}

private:
	char* mem_;
	char* next_;
	char* max_;

	size_t obj_size_;
	size_t pool_size_;

	// keep a stack of freed objects
	size_t* freed_stack_;
	size_t stack_size_;
};

class cpool
{
public:
	cpool(size_t obj_size, size_t max_chunks)
	    : num_chunks_(0), max_chunks_(max_chunks), obj_size_(obj_size)
	{
		init();
	}

	cpool(size_t obj_size)
	    : num_chunks_(0), max_chunks_(20), obj_size_(obj_size)
	{
		init();
	}

	~cpool()
	{
		for(size_t i = 0; i < num_chunks_; i++)
		{
			delete chunks_[i];
		}
		delete[] chunks_;
	}

	inline void
	reclaim()
	{
		for(size_t i = 0; i < num_chunks_; i++)
		{
			chunks_[i]->reclaim();
		}
	}

	inline char*
	allocate()
	{
		char* mem_ptr = current_chunk_->allocate();
		if(!mem_ptr)
		{
			// look for space in an existing chunk
			// NB: linear-time search! increase CHUNK_SIZE_ if
			// number of chunks grows too large
			for(unsigned int i = 0; i < num_chunks_; i++)
			{
				mem_ptr = chunks_[i]->allocate();
				if(mem_ptr)
				{
					current_chunk_ = chunks_[i];
					return mem_ptr;
				}
			}

			// not enough space in any existing chunk; make a new one
			add_chunk(CHUNK_SIZE_);
			current_chunk_ = chunks_[num_chunks_ - 1];
			mem_ptr        = current_chunk_->allocate();
		}
		return mem_ptr;
	}

	inline void
	deallocate(char* addr)
	{
		for(unsigned int i = 0; i < num_chunks_; i++)
		{
			if((unsigned)(addr - chunks_[i]->first_addr())
			   < chunks_[i]->pool_size())
			{
				chunks_[i]->deallocate(addr);
				return;
			}
		}
#ifndef NDEBUG
		std::cerr << "err; cpool::free "
		             "tried to free an address not in any chunk!\n";
#endif
	}

	size_t
	mem()
	{
		size_t bytes = 0;
		for(unsigned int i = 0; i < num_chunks_; i++)
		{
			bytes += chunks_[i]->mem();
		}
		bytes += sizeof(cchunk*) * max_chunks_;
		bytes += sizeof(*this);
		return bytes;
	}

	void
	print(std::ostream& out)
	{
		out << "cpool #chunks: " << num_chunks_ << " #max_chunks "
		    << max_chunks_ << " obj_size: " << obj_size_;
		out << std::endl;
		for(unsigned int i = 0; i < num_chunks_; i++)
		{
			chunks_[i]->print(out);
			out << std::endl;
		}
	}

private:
	cchunk** chunks_;
	cchunk* current_chunk_;

	size_t num_chunks_;
	size_t max_chunks_;
	size_t obj_size_;
	size_t CHUNK_SIZE_;

	// no copy
	cpool(const cpool& other) { }
	cpool&
	operator=(const cpool& other)
	{
		return *this;
	}

	void
	init()
	{
		// chunk size needs to be at least as big as one object
		CHUNK_SIZE_ = std::max<size_t>(obj_size_, DEFAULT_CHUNK_SIZE);

		chunks_ = new cchunk*[max_chunks_];
		for(int i = 0; i < (int)max_chunks_; i++)
		{
			add_chunk(CHUNK_SIZE_);
		}
		current_chunk_ = chunks_[0];
	}

	void
	add_chunk(size_t pool_size)
	{
		if(num_chunks_ < max_chunks_)
		{
			chunks_[num_chunks_] = new cchunk(obj_size_, pool_size);
			num_chunks_++;
		}
		else
		{
			// make room for a new chunk
			size_t big_max      = max_chunks_ * 2;
			cchunk** big_chunks = new cchunk*[big_max];
			for(unsigned int i = 0; i < max_chunks_; i++)
			{
				big_chunks[i] = chunks_[i];
			}
			delete[] chunks_;

			chunks_     = big_chunks;
			max_chunks_ = big_max;

			// finally; add a new chunk
			chunks_[num_chunks_] = new cchunk(obj_size_, pool_size);
			num_chunks_++;
		}
	}
};

} // namespace warthog::memory

#endif // WARTHOG_MEMORY_CPOOL_H
