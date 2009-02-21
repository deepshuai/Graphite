#include "lcp.h"
#include "simulator.h"
#include "core.h"
#include "message_types.h"

#include "log.h"
#define LOG_DEFAULT_RANK -1
#define LOG_DEFAULT_MODULE LCP

// -- general LCP functionality

LCP::LCP()
   : m_proc_num(Config::getSingleton()->getCurrentProcessNum())
   , m_transport(Transport::getSingleton()->getGlobalNode())
   , m_finished(false)
{
}

LCP::~LCP()
{
}

void LCP::run()
{
   LOG_PRINT("LCP started.");

   while (!m_finished)
   {
      processPacket();
   }
}

void LCP::processPacket()
{
   Byte *pkt = m_transport->recv();

   SInt32 *msg_type = (SInt32*)pkt;

   LOG_PRINT("Received message type: %d", *msg_type);

   switch (*msg_type)
   {
   case LCP_MESSAGE_QUIT:
      LOG_PRINT("Received quit message.");
      m_finished = true;
      break;

   case LCP_MESSAGE_COMMID_UPDATE:
      updateCommId(pkt + sizeof(SInt32));
      break;

   default:
      LOG_ASSERT_ERROR(false, "Unexpected message type.");
      break;
   }

   delete [] pkt;
}

void LCP::finish()
{
   LOG_PRINT("Send LCP quit message");

   SInt32 msg_type = LCP_MESSAGE_QUIT;

   m_transport->globalSend(m_proc_num,
                           &msg_type,
                           sizeof(msg_type));

   while (!m_finished)
      sched_yield();

   LOG_PRINT("LCP finished.");
}

// -- functions for specific tasks

struct CommMapUpdate
{
   SInt32 comm_id;
   SInt32 core_id;
};

void LCP::updateCommId(void *vp)
{
   CommMapUpdate *update = (CommMapUpdate*)vp;

   LOG_PRINT("Initializing comm_id: %d to core_id: %d", update->comm_id, update->core_id);
   Config::getSingleton()->updateCommToCoreMap(update->comm_id, update->core_id);

   // FIXME: Do we need to send an ACK?
}
