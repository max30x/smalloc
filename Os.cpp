#include "Os.hpp"

void* os_to_page(void *addr,std::size_t length,bool exec){
	int prot = PROT_READ|PROT_WRITE;
	if (exec)
		prot |= PROT_EXEC;
		
	void* ret = mmap(addr,length,prot,MAP_PRIVATE|MAP_ANON,-1,0);
	
	if (addr!=nullptr && ret!=addr){
		munmap(addr,length);
		return nullptr;
	}
	return ret;
}

int page_to_os(void *addr,std::size_t length){
	return munmap(addr,length);
}

int page_to_corner(void *addr,std::size_t length){
	//todo:maybe i should do this in a better way
	return madvise(addr,length,MADV_FREE);
}

