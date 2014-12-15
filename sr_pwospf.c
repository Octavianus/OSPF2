/*-----------------------------------------------------------------------------
 * file: sr_pwospf.c
 * date: Tue Nov 23 23:24:18 PST 2004
 * Author: Martin Casado
 *
 * Description:
 *
 *---------------------------------------------------------------------------*/

#include "sr_pwospf.h"
#include "sr_router.h"
#include "pwospf_protocol.h"
#include "sr_rt.h"

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <netinet/in.h>

bool if_unable;
struct in_addr router_id;
// static const uint8_t OSPF_DEFAULT_HELLOINT = 5;

struct sr_if_packet
{
	struct sr_instance* sr;
	struct sr_if* interface;
}__attribute__ ((packed));

/* -- declaration of main thread function for pwospf subsystem --- */
void handle_hello_packets(struct sr_instance* sr, struct sr_if* interface, uint8_t* packet, unsigned int length);
void sr_handleLSU(struct sr_instance* sr,
        uint8_t * packet,
        unsigned int len,
        char* interface);
static void* pwospf_run_thread(void* arg);
void add_neighbor(struct if_nbr* nbr_head, struct if_nbr* new_neighbor);
void hello_messages_thread(struct sr_instance *sr);
void* hello_message(struct sr_if_packet * sr_if_pk);
void* scan_neighbor_list(struct sr_instance* sr);
void dijkstra_stack_push(struct route_dijkstra_node* dijkstra_first_item, struct route_dijkstra_node* dijkstra_new_item);
struct route_dijkstra_node* dijkstra_stack_pop(struct route_dijkstra_node* dijkstra_first_item);
struct route_dijkstra_node* create_dikjstra_item(struct pwospf_topology_entry* new_topology_entry, uint8_t dist);
void* run_dijkstra(struct sr_instance* sr);

void sr_handlePWOSPF(struct sr_instance* sr,
        uint8_t * packet,
        unsigned int len,
        char* interface)
{
	struct ospfv2_hdr *pw_hdr = (struct ospfv2_hdr *)(packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip));
	if (pw_hdr->type == OSPF_TYPE_HELLO)
		handle_hello_packets(sr, interface, packet, len);
	//if (pw_hdr->type == OSPF_TYPE_LSU)
		//sr_handleLSU(sr, packet,len,interface);

}

// ALLSPFRouters that is defined as "224.0.0.5", Mac addr of this IP is 0x01, 0x00, 0x5e, 0x00, 0x00, 0x05
uint8_t hello_broadcast_addr[ETHER_ADDR_LEN] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x05};


/*---------------------------------------------------------------------
 * Method: pwospf_init(..)
 *
 * Sets up the internal data structures for the pwospf subsystem
 *
 * You may assume that the interfaces have been created and initialized
 * by this point.
 *---------------------------------------------------------------------*/

int pwospf_init(struct sr_instance* sr)
{
	assert(sr);

	sr->ospf_subsys = (struct pwospf_subsys*)malloc(sizeof(struct pwospf_subsys));

	assert(sr->ospf_subsys);
	pthread_mutex_init(&(sr->ospf_subsys->lock), 0);
	if_unable = false;

	// nbr list header pointer and init header address.
	struct in_addr header_addr;
	header_addr.s_addr = 0;
	struct sr_if* if_walker = sr->if_list;
	while(if_walker != NULL){
		if_walker->nbr_list = ((struct if_nbr*)(malloc(sizeof(struct if_nbr))));
		if_walker->nbr_list->nbr_id = header_addr.s_addr;
		if_walker->nbr_list->alive = OSPF_NEIGHBOR_TIMEOUT;
		if_walker->nbr_list->next = NULL;
		if_walker = if_walker->next;
	}

	/* -- handle subsystem initialization here! -- */

	// TODO need it
	// topology_header = create_ospfv2_topology_entry(header_addr, header_addr, header_addr, header_addr, header_addr, 0);

	/* -- start thread subsystem -- */
	if( pthread_create(&sr->ospf_subsys->thread, 0, pwospf_run_thread, sr)) {
		perror("pthread_create");
		assert(0);
	}

	if( pthread_create(&T_hello, NULL, hello_messages_thread, sr)) {
		perror("pthread_create");
		assert(0);
	}

	if( pthread_create(&T_neighbor, NULL, scan_neighbor_list, sr)) {
		perror("pthread_create");
		assert(0);
	}

	return 0; /* success */
} /* -- pwospf_init -- */


/*---------------------------------------------------------------------
 * Method: pwospf_lock
 *
 * Lock mutex associated with pwospf_subsys
 *
 *---------------------------------------------------------------------*/

void pwospf_lock(struct pwospf_subsys* subsys)
{
	if ( pthread_mutex_lock(&subsys->lock) )
	{ assert(0); }
} /* -- pwospf_subsys -- */

/*---------------------------------------------------------------------
 * Method: pwospf_unlock
 *
 * Unlock mutex associated with pwospf subsystem
 *
 *---------------------------------------------------------------------*/

void pwospf_unlock(struct pwospf_subsys* subsys)
{
	if ( pthread_mutex_unlock(&subsys->lock) )
	{ assert(0); }
} /* -- pwospf_subsys -- */

/*---------------------------------------------------------------------
 * Method: pwospf_run_thread
 *
 * Main thread of pwospf subsystem.
 *
 *---------------------------------------------------------------------*/

static
void* pwospf_run_thread(void* arg)
{
	struct sr_instance* sr = (struct sr_instance*)arg;

	while(1)
	{
		/* -- PWOSPF subsystem functionality should start  here! -- */

		pwospf_lock(sr->ospf_subsys);
		printf(" pwospf subsystem sleeping \n");
		pwospf_unlock(sr->ospf_subsys);
		sleep(2);
		printf(" pwospf subsystem awake \n");
	};
} /* -- run_ospf_thread -- */

/*------------------------------------------------------------------------------------
 * Method: handle_hello_packets(struct sr_instance* sr,
 * struct sr_if* interface,
 * uint8_t* packet,
 * unsigned int length)
 * Handle the hello packets that send from other routers.
 *-----------------------------------------------------------------------------------*/
void handle_hello_packets(struct sr_instance* sr, struct sr_if* interface, uint8_t* packet, unsigned int length)
{
	bool exit_neighbor = true;
	uint16_t checksum = 0;
	struct in_addr neighbor_id;
	// Contruct the header.

	struct sr_if *sr_in = sr_get_interface(sr, interface);
	struct in_addr tempid;
	tempid.s_addr = sr_get_interface(sr, "eth0")->ip;
	router_id.s_addr = tempid.s_addr;
	struct ip * iP_Hdr = NULL;
	iP_Hdr = (struct ip *)(packet + sizeof(struct sr_ethernet_hdr));
	struct ospfv2_hdr* ospfv2_Hdr = (struct ospfv2_hdr *)(packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip));
	struct ospfv2_hello_hdr* ospfv2_Hello_Hdr = (struct ospfv2_hello_hdr *)(packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip) + sizeof(struct ospfv2_hdr));

	neighbor_id.s_addr = ospfv2_Hdr->rid;
	struct in_addr net_mask;
	net_mask.s_addr = ospfv2_Hello_Hdr->nmask;
	Debug("-> PWOSPF: Detecting PWOSPF HELLO Packet from:\n");
	Debug("      [Neighbor ID = %s]\n", inet_ntoa(neighbor_id));
	Debug("      [Neighbor IP = %s]\n", inet_ntoa(iP_Hdr->ip_src));
	Debug("      [Network Mask = %s]\n", inet_ntoa(net_mask));

	// Examine the checksum
	uint16_t rx_checksum = 0;
	rx_checksum = ospfv2_Hdr->csum;
	uint8_t * hdr_checksum = ((uint8_t *)(packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip)));
	checksum = cal_ICMPcksum(hdr_checksum, sizeof(struct ospfv2_hello_hdr) + sizeof(struct ospfv2_hdr));

	/*if (checksum != rx_checksum)
	{
		Debug("-> PWOSPF: HELLO Packet dropped, invalid checksum\n");
		return;
	}
	*/

	// Examine the validation of the hello interval
	if (ospfv2_Hello_Hdr->helloint != htons(OSPF_DEFAULT_HELLOINT))
	{
		Debug("-> PWOSPF: HELLO Packet dropped, invalid hello interval\n");
		return;
	}

	// Examine the interface mask, 
	if (htonl(ospfv2_Hello_Hdr->nmask) != sr_in->mask)
	{
		Debug("-> PWOSPF: HELLO Packet dropped, invalid hello network mask\n");
		return;
	}

	// If it is already the neighbor of the interface, which Interface receive the packet records the id and ip
	struct if_nbr* nbr_walker = sr_in->nbr_list;
	while(nbr_walker != NULL)
	{
		if (nbr_walker->nbr_id != ospfv2_Hdr->rid)
		{
			nbr_walker = nbr_walker->next;
			if(nbr_walker == NULL)
				exit_neighbor = false;
		}else
		{
			nbr_walker->nbr_ip = iP_Hdr->ip_src.s_addr;
			break;
		}
	}
	
	// give the header of the neighbor list and the neighbor id, and renew the neighbors' timestamp
	// Need to scan all the neighbor because, each receive proving its alive
	struct sr_if* if_walker = sr->if_list;
	while(if_walker != NULL)
	{
		struct if_nbr* ptr = if_walker->nbr_list;
		while(ptr != NULL)
		{
			if (ptr->nbr_id == neighbor_id.s_addr)
			{
				Debug("-> PWOSPF: Refreshing the neighbor, [ID = %s] in the alive neighbors table\n", inet_ntoa(neighbor_id));
				ptr->alive = OSPF_NEIGHBOR_TIMEOUT;
				break;
			}
	
			ptr = ptr->next;
		}
		if_walker = if_walker->next;
	}

	// send the lsu announcement to the internet of adding a new neighbor
	if (exit_neighbor == false)
	{
		// Create a new neighbor and malloc memory for it, and add it to the list
		struct if_nbr* new_neighbor = (struct if_nbr*)(malloc(sizeof(struct if_nbr)));
		new_neighbor->nbr_id = neighbor_id.s_addr;
		new_neighbor->nbr_ip = iP_Hdr->ip_src.s_addr;
		new_neighbor->alive = OSPF_NEIGHBOR_TIMEOUT;
		new_neighbor->next = NULL;
		// sub function of creat a new neib, memory for new neighbor is allocated
		// Add a new node in interface neighbor list
		add_neighbor(sr_in->nbr_list, new_neighbor);
		Debug("-> PWOSPF: Adding the neighbor, [ID = %s] to the alive neighbors table\n", inet_ntoa(neighbor_id));
		struct sr_if_packet* lsu_param = (struct sr_if_packet*)(malloc(sizeof(struct sr_if_packet)));
		lsu_param->sr = sr;
		lsu_param->interface = interface;
		//pthread_create(&lsu_thread, NULL, send_lsu, lsu_param);
	}
}


/*------------------------------------------------------------------------------------
 * Method: hello_messages_thread(struct sr_instance *sr)
 * Periodically check the interface list of the router to decide whether to send hello
 *-----------------------------------------------------------------------------------*/
void hello_messages_thread(struct sr_instance *sr)
{
	//struct sr_instance* sr = (struct sr_instance*)arg;

	while(1)
	{
		sleep(OSPF_DEFAULT_HELLOINT);
		pwospf_lock(sr->ospf_subsys);

		// Interate all the interface
		struct sr_if* if_walker = sr->if_list;
		while(if_walker != NULL)
		{
			// TODO check if this is necessary
			/*
			// Check if this interface is down
			if (if_unable == true)
			{
				// Skip this down interface
				if (strcmp(if_walker->name, sr->f_interface) == 0)
				{
					if_walker = if_walker->next;
					continue;
				}
			}

			// Reduce the helloint of the unreceived interface
			if (if_walker->helloint > 0)
			{
				if_walker->helloint--;
			}
			else
			{
			*/
			// send hello packet.
			struct sr_if_packet* sr_if_pk = (struct sr_if_packet*)(malloc(sizeof(struct sr_if_packet)));
			sr_if_pk->sr = sr;
			sr_if_pk->interface = if_walker;
			pthread_create(&T_hello, NULL, hello_message, sr_if_pk); // void sr_if_pk
			//pthread_create( &thread, NULL, Arp_Cache_Timeout, (void*)&sr_if_pk);
			//if_walker->helloint = OSPF_DEFAULT_HELLOINT;
			//}
			if_walker = if_walker->next;
		}

		pwospf_unlock(sr->ospf_subsys);
	};

}

/*------------------------------------------------------------------------------------
 * Method: hello_message(sr_if_packet * sr_if_pk)
 * Detail to build the packet and send it by calling sr_send_packet.
 *-----------------------------------------------------------------------------------*/
void* hello_message(struct sr_if_packet * sr_if_pk)
{
	uint8_t* hello_packet = NULL;
	int packet_len = sizeof( struct sr_ethernet_hdr) + sizeof(struct ip) + sizeof(struct ospfv2_hdr) + sizeof(struct ospfv2_hello_hdr);
	hello_packet = ((uint8_t*)(malloc(packet_len)));

	Debug("\n\nPWOSPF: Constructing HELLO packet for interface %s: \n", sr_if_pk->interface->name);

	struct sr_ethernet_hdr* eth_hdr = (struct sr_ethernet_hdr*)(malloc(sizeof(struct sr_ethernet_hdr)));
	struct ip* ip_hdr = (struct ip*)(malloc(sizeof(struct ip)));
	struct ospfv2_hdr* ospf_hdr = (struct ospfv2_hdr*)(malloc(sizeof(struct ospfv2_hdr)));
	struct ospfv2_hello_hdr* hello_hdr = (struct ospfv2_hello_hdr*)(malloc(sizeof(struct ospfv2_hello_hdr)));

	// Copy the destination and source mac address from the target.
	for (int i = 0; i < ETHER_ADDR_LEN; i++)
	{
		eth_hdr->ether_dhost[i] = hello_broadcast_addr[i];
		eth_hdr->ether_shost[i] = (uint8_t)(sr_if_pk->interface->addr[i]);
	}

	eth_hdr->ether_type = htons(ETHERTYPE_IP);
	ip_hdr->ip_v = (sizeof(struct ip))/4;
	ip_hdr->ip_hl = 5;
	ip_hdr->ip_tos = 0;
	ip_hdr->ip_len = htons((sizeof(struct ip)) + sizeof(struct ospfv2_hdr) + sizeof(struct ospfv2_hello_hdr));

	ip_hdr->ip_id = 0; /* 0 */
	ip_hdr->ip_sum = 0;
	ip_hdr->ip_off = 0;
	ip_hdr->ip_ttl = 64;
	ip_hdr->ip_p = 89; // OSPFv2

	ip_hdr->ip_src.s_addr = sr_if_pk->interface->ip;
	ip_hdr->ip_dst.s_addr = htonl(OSPF_AllSPFRouters);
	// Recalculate the checksum of the ip header.
	ip_hdr->ip_sum = cal_ICMPcksum((uint8_t*)(ip_hdr), sizeof(struct ip));

	ospf_hdr->version = OSPF_V2;
	ospf_hdr->type = OSPF_TYPE_HELLO;
	ospf_hdr->len = htons(sizeof(struct ospfv2_hdr) + sizeof(struct ospfv2_hello_hdr));
	ospf_hdr->rid = router_id.s_addr;    //It is the highest IP address on a router [according to Cisco]
	ospf_hdr->aid = htonl(171); //TODO now the area id dynamically
	ospf_hdr->csum = 0;
	ospf_hdr->autype = 0;
	ospf_hdr->audata = 0;
	hello_hdr->nmask = htonl(sr_if_pk->interface->mask);
	hello_hdr->helloint = htons(OSPF_DEFAULT_HELLOINT);
	hello_hdr->padding = 0;

	// Create the packet
	// Copy the content to the packet.
	memcpy(hello_packet, eth_hdr, sizeof(struct sr_ethernet_hdr));
	memcpy(hello_packet + sizeof(struct sr_ethernet_hdr), ip_hdr, sizeof(struct ip));
	memcpy(hello_packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip), ospf_hdr, sizeof(struct ospfv2_hdr));
	memcpy(hello_packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip) + sizeof(struct ospfv2_hdr), hello_hdr, sizeof(struct ospfv2_hello_hdr));

	// Update the ospf2 header checksum.
	//uint8_t * temp_packet = hello_packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip);
	//ospf_hdr->csum = cal_IPcksum((uint8_t*)temp_packet, sizeof(struct ospfv2_hdr) + sizeof(struct ospfv2_hello_hdr));
	//ospf_hdr->csum = htons(ospf_hdr->csum);
	((struct ospfv2_hdr *)(hello_packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip)))->csum =
			cal_ICMPcksum(hello_packet + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip), sizeof(struct ospfv2_hdr) + sizeof(struct ospfv2_hello_hdr));

	Debug("-> PWOSPF: Sending HELLO Packet of length = %d, out of the interface: %s\n", packet_len, sr_if_pk->interface->name);
	sr_send_packet(sr_if_pk->sr, (uint8_t*)(hello_packet), packet_len, sr_if_pk->interface->name);

}

/*------------------------------------------------------------------------------------
 * Method: scan_neighbor_list(void* thread)
 * Scan the neighbor list, check if neighbors are alive or deleted them for the list.
 *-----------------------------------------------------------------------------------*/
void* scan_neighbor_list(struct sr_instance* sr)
{
	while(1)
	{
		usleep(1000000);
		
		struct sr_if* if_walker = sr->if_list;
		while(if_walker != NULL){
			struct if_nbr* tmp_walker = if_walker->nbr_list;
			while(tmp_walker != NULL)
			{
				// If there is no neighbor in this interface, break from the scan.
				if (tmp_walker->next == NULL)
				{
					break;
				}
	
				// if the alive is zero then delete the neighbor from the list.
				if (tmp_walker->next->alive == 0)
				{
					// Debug("\n\n**** PWOSPF: Removing the neighbor, [ID = %s] from the alive neighbors table\n\n", inet_ntoa(ptr->next->neighbor_id));
	
					struct if_nbr* delete_neighbor = tmp_walker->next;
	
					if (tmp_walker->next->next != NULL)
					{
						tmp_walker->next = tmp_walker->next->next;
					}
					else
					{
						tmp_walker->next = NULL;
					}
	
					free(delete_neighbor);
				}
				else
				{
					// else deduce the alive for one.
					tmp_walker->next->alive--;
				}
	
				tmp_walker = tmp_walker->next;
			}
			if_walker = if_walker->next;
		}
	};

}

/*------------------------------------------------------------------------------------
 * Method: dijkstra_stack_push(struct route_dijkstra_node* dijkstra_first_item, struct route_dijkstra_node* dijkstra_new_item)
 * Push a visited node into the stack structure to calculate the dijkstra
 *-----------------------------------------------------------------------------------*/
void dijkstra_stack_push(struct route_dijkstra_node* dijkstra_first_item, struct route_dijkstra_node* dijkstra_new_item)
{
	if (dijkstra_first_item->next != NULL)
	{
		dijkstra_new_item->next = dijkstra_first_item->next;
		dijkstra_first_item->next = dijkstra_new_item;
	}

	dijkstra_first_item->next = dijkstra_new_item;
}

/*------------------------------------------------------------------------------------
 * Method: dijkstra_stack_pop(struct route_dijkstra_node* dijkstra_first_item)
 * Pop a visited node into the stack structure to calculate the dijkstra
 *-----------------------------------------------------------------------------------*/
struct route_dijkstra_node* dijkstra_stack_pop(struct route_dijkstra_node* dijkstra_first_item)
{
	if (dijkstra_first_item->next == NULL)
	{
		return NULL;
	}
	else
	{
		struct route_dijkstra_node* pResult = dijkstra_first_item->next;

		dijkstra_first_item->next = dijkstra_first_item->next->next;

		return pResult;
	}
}

/*------------------------------------------------------------------------------------
 * Method: create_dikjstra_item(struct pwospf_topology_entry* new_topology_entry, uint8_t dist)
 * Create a new dijkstra list to save the router's node.
 *-----------------------------------------------------------------------------------*/
struct route_dijkstra_node* create_dikjstra_item(struct pwospf_topology_entry* new_topology_entry, uint8_t dist)
{
	struct route_dijkstra_node* dijkstra_new_item = ((struct route_dijkstra_node*)(malloc(sizeof(struct route_dijkstra_node))));
	dijkstra_new_item->topology_entry = new_topology_entry;
	dijkstra_new_item->dist = dist;
	dijkstra_new_item->prev = NULL;
	dijkstra_new_item->next = NULL;
	return dijkstra_new_item;
}

/*------------------------------------------------------------------------------------
 * Method: (neighbor_list* ngh_head, neighbor_list* new_neighbor)
 * Add a neighbor to the neighbor list
 *-----------------------------------------------------------------------------------*/
void add_neighbor(struct if_nbr* ngh_head, struct if_nbr* new_neighbor)
{
    if (ngh_head->next != NULL)
    {
        new_neighbor->next = ngh_head->next;
        ngh_head->next = new_neighbor;
    }
    else
    {
    	ngh_head->next = new_neighbor;
    }
}

/*---------------------------------------------------------------------
 * Method: run_dijkstra(struct sr_instance* sr)
 *
 * Run Dijkstra algorithm, Let the node at which we are starting be
 * called the initial node. Let the distance of node Y be the distance from the initial node to Y.
 *---------------------------------------------------------------------*/

