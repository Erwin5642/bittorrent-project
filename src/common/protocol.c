#include <stdint.h>
#include <sys/types.h>



//TODO:  types of each attribute to be decided
typedef struct payloadHeader{
	uint32_t protocol_ver;
	uint32_t msg_type;
	uint32_t src_node;
	uint32_t dst_node;
	uint32_t trsc_id;
	uint32_t time;
	uint32_t pl_size;
	uint32_t checksun;
}pl_header;
