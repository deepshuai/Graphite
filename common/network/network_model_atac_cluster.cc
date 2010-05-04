#include <math.h>
#include <vector>
using namespace std;

#include "network_model_atac_cluster.h"
#include "simulator.h"
#include "core_manager.h"
#include "core.h"
#include "config.h"
#include "log.h"
#include "memory_manager_base.h"
#include "packet_type.h"

SInt32 NetworkModelAtacCluster::m_sqrt_cluster_size = 0;
SInt32 NetworkModelAtacCluster::m_mesh_width = 0;
SInt32 NetworkModelAtacCluster::m_mesh_height = 0;

NetworkModelAtacCluster::NetworkModelAtacCluster(Network *net) :
   NetworkModel(net),
   m_enabled(false)
{
   m_core_id = getNetwork()->getCore()->getId();
   m_num_application_cores = Config::getSingleton()->getApplicationCores(); 
   m_total_cores = Config::getSingleton()->getTotalCores();
  
   SInt32 cluster_size = 0; 
   try
   {
      // Optical delay is specified in 'clock cycles'
      m_optical_hop_latency = (UInt64) Sim()->getCfg()->getInt("network/atac_cluster/optical_hop_latency");
      m_electrical_mesh_hop_latency = (UInt64) Sim()->getCfg()->getInt("network/atac_cluster/electrical_mesh_hop_latency");

      // Optical Bandwidth is specified in 'bits/clock cycle'
      // In other words, it states the number of wavelengths used
      m_optical_bus_bandwidth = Sim()->getCfg()->getInt("network/atac_cluster/optical_bus_bandwidth");
      m_num_electrical_broadcast_networks_per_cluster = Sim()->getCfg()->getInt("network/atac_cluster/num_electrical_broadcast_networks_per_cluster");

      // Cluster Size
      cluster_size = Sim()->getCfg()->getInt("network/atac_cluster/cluster_size");
     
      // Queue Models 
      m_queue_model_enabled = Sim()->getCfg()->getBool("network/atac_optical_bus/queue_model/enabled");
      m_queue_model_type = Sim()->getCfg()->getString("network/atac_optical_bus/queue_model/type");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error reading atac network model parameters");
   }

   // Cluster Size
   m_sqrt_cluster_size = (SInt32) floor(sqrt(cluster_size));
   LOG_ASSERT_ERROR(cluster_size == (m_sqrt_cluster_size * m_sqrt_cluster_size),
         "Cluster Size(%i) must be a perfect square", cluster_size);

   m_sender_cluster_electrical_network_delay = m_electrical_mesh_hop_latency * ceil(1.0 * m_sqrt_cluster_size / 2);
   m_receiver_cluster_electrical_network_delay = m_electrical_mesh_hop_latency * ceil(1.0 * m_sqrt_cluster_size / 2);

   // Calculations with an electrical mesh
   m_mesh_width = (SInt32) floor(sqrt(m_total_cores));
   m_mesh_height = (SInt32) ceil(1.0 * m_total_cores / m_mesh_width);
   LOG_ASSERT_ERROR(m_mesh_width % m_sqrt_cluster_size == 0,
         "Mesh Width(%i) must be a multiple of sqrt_cluster_size(%i)", m_mesh_width, m_sqrt_cluster_size);
   LOG_ASSERT_ERROR(m_mesh_height == (m_mesh_width + 1),
         "Mesh Width(%i), Mesh Height(%i)", m_mesh_width, m_mesh_height);
   LOG_ASSERT_ERROR((m_mesh_width * m_mesh_height) == (SInt32) m_total_cores,
         "Mesh Width(%i), Mesh Height(%i), Core Count(%i)", m_mesh_width, m_mesh_height, m_total_cores);
   
   m_num_clusters = (m_mesh_width / m_sqrt_cluster_size) * ceil(1.0 * m_mesh_height / m_sqrt_cluster_size);

   // Optical Hub created only on one of the cores in the cluster
   createOpticalHub();

   // Performance Counters
   initializePerformanceCounters();
}

NetworkModelAtacCluster::~NetworkModelAtacCluster()
{
   destroyOpticalHub();
}

void
NetworkModelAtacCluster::initializePerformanceCounters()
{
   m_total_bytes_received = 0;
   m_total_packets_received = 0;
   m_total_packet_latency = 0;
}


UInt64
NetworkModelAtacCluster::computeHubQueueDelay(HubType hub_type, SInt32 sender_cluster_id, UInt64 pkt_time, UInt32 pkt_length)
{
   assert(m_queue_model_enabled);
   assert(m_core_id == getCoreIDWithOpticalHub(getClusterID(m_core_id)));
   assert(m_sender_hub_queue_model);
   assert(m_receiver_hub_queue_models);

   switch (hub_type)
   {
      case SENDER_HUB:
         {
            ScopedLock sl(m_sender_hub_lock);
            
            UInt64 processing_time = computeProcessingTime(pkt_length, m_optical_bus_bandwidth);

            LOG_ASSERT_ERROR(sender_cluster_id == getClusterID(m_core_id), "sender_cluster_id(%i), curr_cluster_id(%i)",
                  sender_cluster_id, getClusterID(m_core_id));
            UInt64 sender_hub_queue_delay = m_sender_hub_queue_model->computeQueueDelay(pkt_time, processing_time);

            // Performance Counters
            m_total_sender_hub_contention_delay += sender_hub_queue_delay;
            m_total_sender_hub_packets ++;
            return sender_hub_queue_delay;
         }

      case RECEIVER_HUB:
         {
            ScopedLock sl(m_receiver_hub_lock);
            
            UInt64 processing_time = computeProcessingTime(pkt_length, m_optical_bus_bandwidth);

            // Assume the broadcast networks are statically divided up among the senders
            SInt32 broadcast_network_num = sender_cluster_id % m_num_electrical_broadcast_networks_per_cluster;
            UInt64 receiver_hub_queue_delay = m_receiver_hub_queue_models[broadcast_network_num]->computeQueueDelay(pkt_time, processing_time);

            // Performance Counters
            m_total_receiver_hub_contention_delay[broadcast_network_num] += receiver_hub_queue_delay;
            m_total_receiver_hub_packets[broadcast_network_num] ++;
            return receiver_hub_queue_delay;
         }

      default:
         LOG_PRINT_ERROR("Unrecognized Hub Type(%u)", hub_type);
         return 0;
   }
}

UInt64 
NetworkModelAtacCluster::computeProcessingTime(UInt32 pkt_length, UInt32 bandwidth)
{
   // Send: (pkt_length * 8) bits
   // Bandwidth: (m_optical_bandwidth) bits/cycle
   UInt32 num_bits = pkt_length * 8;
   if (num_bits % bandwidth == 0)
      return (UInt64) (num_bits/bandwidth);
   else
      return (UInt64) (num_bits/bandwidth + 1);
}

core_id_t
NetworkModelAtacCluster::getRequester(const NetPacket& pkt)
{
   core_id_t requester = INVALID_CORE_ID;

   if ((pkt.type == SHARED_MEM_1) || (pkt.type == SHARED_MEM_2))
      requester = getNetwork()->getCore()->getMemoryManager()->getShmemRequester(pkt.data);
   else // Other Packet types
      requester = pkt.sender;

   LOG_ASSERT_ERROR((requester >= 0) && (requester < (core_id_t) Config::getSingleton()->getTotalCores()),
         "requester(%i)", requester);
   
   return requester;
}

UInt64
NetworkModelAtacCluster::getHubQueueDelay(HubType hub_type, SInt32 sender_cluster_id, SInt32 cluster_id, UInt64 pkt_time, UInt32 pkt_length, PacketType pkt_type, core_id_t requester)
{
   if ((!m_enabled) || (!m_queue_model_enabled) || (requester >= (core_id_t) m_num_application_cores))
      return 0;

   // Components
   // 1) sender_cluster_electrical_network_latency
   // 2) sender_hub_queue_delay
   // 3) optical_latency

   core_id_t core_id_with_optical_hub = getCoreIDWithOpticalHub(cluster_id);
   Core* core = Sim()->getCoreManager()->getCoreFromID(core_id_with_optical_hub);
   NetworkModelAtacCluster* network_model = (NetworkModelAtacCluster*) core->getNetwork()->getNetworkModelFromPacketType(pkt_type);

   return network_model->computeHubQueueDelay(hub_type, sender_cluster_id, pkt_time, pkt_length);
}

void 
NetworkModelAtacCluster::routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops)
{
   ScopedLock sl(m_lock);

   core_id_t requester = getRequester(pkt);

   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);
   UInt64 processing_time = computeProcessingTime(pkt_length, m_optical_bus_bandwidth);

   if (pkt.receiver == NetPacket::BROADCAST)
   {
      UInt64 sender_hub_queue_delay = getHubQueueDelay(SENDER_HUB, getClusterID(pkt.sender), getClusterID(pkt.sender),
            pkt.time + m_sender_cluster_electrical_network_delay, pkt_length, pkt.type, requester);
      UInt64 latency_sender_core_to_receiver_hub = m_sender_cluster_electrical_network_delay + sender_hub_queue_delay + m_optical_hop_latency;
      
      UInt64 receiver_hub_queue_delay[m_num_clusters];
      for (SInt32 i = 0; i < (SInt32) m_num_clusters; i++)
      {
         receiver_hub_queue_delay[i] = getHubQueueDelay(RECEIVER_HUB, getClusterID(pkt.sender), i,
               pkt.time + latency_sender_core_to_receiver_hub, pkt_length, pkt.type, requester);
      }

      for (core_id_t i = 0; i < (core_id_t) m_total_cores; i++)
      {
         UInt64 latency_receiver_hub_to_receiver_core = receiver_hub_queue_delay[getClusterID(i)] + m_receiver_cluster_electrical_network_delay;

         UInt64 total_latency = latency_sender_core_to_receiver_hub + latency_receiver_hub_to_receiver_core + processing_time;

         Hop h;
         h.next_dest = i;
         h.final_dest = i;
         h.time = pkt.time + total_latency;
         
         nextHops.push_back(h);
      }
   }
   else
   {
      // Right now, it is a unicast
      LOG_ASSERT_ERROR(pkt.receiver < (core_id_t) m_total_cores, "Got invalid receiver ID = %i", pkt.receiver);

      UInt64 total_latency;
      if (pkt.sender == pkt.receiver)
      {
         total_latency = 0;
      }
      else if (getClusterID(pkt.sender) == getClusterID(pkt.receiver))
      {
         total_latency = m_sender_cluster_electrical_network_delay + m_receiver_cluster_electrical_network_delay + processing_time;
      }
      else
      {
         UInt64 sender_hub_queue_delay = getHubQueueDelay(SENDER_HUB, getClusterID(pkt.sender), getClusterID(pkt.sender),
               pkt.time + m_sender_cluster_electrical_network_delay, pkt_length, pkt.type, requester);
         UInt64 latency_sender_core_to_receiver_hub = m_sender_cluster_electrical_network_delay + sender_hub_queue_delay + m_optical_hop_latency;

         UInt64 receiver_hub_queue_delay = getHubQueueDelay(RECEIVER_HUB, getClusterID(pkt.sender), getClusterID(pkt.receiver),
               pkt.time + latency_sender_core_to_receiver_hub, pkt_length, pkt.type, requester);
         UInt64 latency_receiver_hub_to_receiver_core = receiver_hub_queue_delay + m_receiver_cluster_electrical_network_delay;

         total_latency = latency_sender_core_to_receiver_hub + latency_receiver_hub_to_receiver_core + processing_time;
      }

      Hop h;
      h.next_dest = pkt.receiver;
      h.final_dest = pkt.receiver;
      h.time = pkt.time + total_latency;
      
      nextHops.push_back(h);
   }
}

void
NetworkModelAtacCluster::processReceivedPacket(NetPacket& pkt)
{
   ScopedLock sl(m_lock);
 
   core_id_t requester = getRequester(pkt);

   if ((!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()))
      return;

   // Performance Counters
   m_total_packets_received ++;
   
   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);
   m_total_bytes_received += (UInt64) pkt_length;
   
   UInt64 packet_latency = pkt.time - pkt.start_time;
   m_total_packet_latency += packet_latency;
}

void
NetworkModelAtacCluster::createOpticalHub()
{
   if ((m_queue_model_enabled) && (m_core_id == getCoreIDWithOpticalHub(getClusterID(m_core_id))))
   {
      UInt64 min_processing_time = 1;
      m_sender_hub_queue_model = QueueModel::create(m_queue_model_type, min_processing_time);
      
      m_receiver_hub_queue_models = new QueueModel*[m_num_electrical_broadcast_networks_per_cluster];
      m_total_receiver_hub_contention_delay = new UInt64[m_num_electrical_broadcast_networks_per_cluster];
      m_total_receiver_hub_packets = new UInt64[m_num_electrical_broadcast_networks_per_cluster];
      
      for (SInt32 i = 0; i < (SInt32) m_num_electrical_broadcast_networks_per_cluster; i++)
      {
         m_receiver_hub_queue_models[i] = QueueModel::create(m_queue_model_type, min_processing_time);
         m_total_receiver_hub_contention_delay[i] = 0;
         m_total_receiver_hub_packets[i] = 0;
      }
      m_total_sender_hub_contention_delay = 0;
      m_total_sender_hub_packets = 0; 
   }
   else
   {
      m_sender_hub_queue_model = (QueueModel*) NULL;
      m_receiver_hub_queue_models = (QueueModel**) NULL;
   }
}

void
NetworkModelAtacCluster::destroyOpticalHub()
{
   if ((m_queue_model_enabled) && (m_core_id == getCoreIDWithOpticalHub(getClusterID(m_core_id))))
   {
      delete m_sender_hub_queue_model;
      
      for (SInt32 i = 0; i < (SInt32) m_num_electrical_broadcast_networks_per_cluster; i++)
         delete m_receiver_hub_queue_models[i];
      delete m_receiver_hub_queue_models;

      delete m_total_receiver_hub_contention_delay;
      delete m_total_receiver_hub_packets;
   }
}

void
NetworkModelAtacCluster::outputHubSummary(ostream& out)
{
   out << " ATAC Cluster: " << endl;
   if ((m_queue_model_enabled) && (m_core_id == getCoreIDWithOpticalHub(getClusterID(m_core_id))))
   {
      if (m_total_sender_hub_packets > 0)
         out << "    Sender Hub Contention Delay: " << (float) m_total_sender_hub_contention_delay / m_total_sender_hub_packets << endl;
      else
         out << "    Sender Hub Contention Delay: 0" << endl;
    
      for (UInt32 i = 0; i < m_num_electrical_broadcast_networks_per_cluster; i++)
      {
         if (m_total_receiver_hub_packets[i] > 0)
            out << "    Receiver Hub (" << i << ") Contention Delay: " << ((float) m_total_receiver_hub_contention_delay[i]) / m_total_receiver_hub_packets[i] << endl;
         else
            out << "    Receiver Hub (" << i << ") Contention Delay: 0" << endl;
      }
   }
   else
   {
      out << "    Sender Hub Contention Delay: NA" << endl;
      for (UInt32 i = 0; i < m_num_electrical_broadcast_networks_per_cluster; i++)
      {
         out << "    Receiver Hub (" << i << ") Contention Delay: NA" << endl;
      }
   }
}

void
NetworkModelAtacCluster::outputSummary(ostream &out)
{
   out << "    bytes received: " << m_total_bytes_received << endl;
   out << "    packets received: " << m_total_packets_received << endl;
   if (m_total_packets_received > 0)
      out << "    average packet latency: " << ((float) m_total_packet_latency / m_total_packets_received) << endl;
   else
      out << "    average packet latency: 0" << endl;

   outputHubSummary(out);
}

void
NetworkModelAtacCluster::enable()
{
   m_enabled = true;
}

void
NetworkModelAtacCluster::disable()
{
   m_enabled = false;
}

pair<bool, vector<core_id_t> >
NetworkModelAtacCluster::computeMemoryControllerPositions(SInt32 num_memory_controllers, SInt32 core_count)
{
   // Just a check to see if it is initialized
   assert(m_sqrt_cluster_size > 0);
   assert(m_mesh_width > 0);

   // Get the cluster size, Get the number of clusters
   // Here we only include complete clusters, we dont include the incomplete clusters
   
   SInt32 num_clusters = (m_mesh_width / m_sqrt_cluster_size) * (m_mesh_width / m_sqrt_cluster_size);
   LOG_ASSERT_ERROR(num_memory_controllers <= num_clusters,
         "num_memory_controllers(%i), num_clusters(%i)", num_memory_controllers, num_clusters);

   vector<core_id_t> core_id_list_with_memory_controllers;
   for (SInt32 i = 0; i < num_memory_controllers; i++)
   {
      core_id_list_with_memory_controllers.push_back(getCoreIDWithOpticalHub(i));
   }

   return (make_pair(true, core_id_list_with_memory_controllers));
}

SInt32
NetworkModelAtacCluster::getClusterID(core_id_t core_id)
{
   // Consider a mesh formed by the clusters
   SInt32 cluster_mesh_width;
   cluster_mesh_width = m_mesh_width / m_sqrt_cluster_size;

   SInt32 core_x, core_y;
   core_x = core_id % m_mesh_width;
   core_y = core_id / m_mesh_width;

   SInt32 cluster_pos_x, cluster_pos_y;
   cluster_pos_x = core_x / m_sqrt_cluster_size;
   cluster_pos_y = core_y / m_sqrt_cluster_size;

   return (cluster_pos_y * cluster_mesh_width + cluster_pos_x);
}

core_id_t
NetworkModelAtacCluster::getCoreIDWithOpticalHub(SInt32 cluster_id)
{
   // Consider a mesh formed by the clusters
   SInt32 cluster_mesh_width;
   cluster_mesh_width = m_mesh_width / m_sqrt_cluster_size;

   SInt32 cluster_pos_x, cluster_pos_y;
   cluster_pos_x = cluster_id % cluster_mesh_width;
   cluster_pos_y = cluster_id / cluster_mesh_width;

   SInt32 optical_hub_x, optical_hub_y; 
   optical_hub_x = cluster_pos_x * m_sqrt_cluster_size;
   optical_hub_y = cluster_pos_y * m_sqrt_cluster_size;

   return (optical_hub_y * m_mesh_width + optical_hub_x);
}
