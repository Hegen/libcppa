#include "cppa/detail/mailman.hpp"
#include "cppa/binary_serializer.hpp"

// forward declaration
namespace cppa { namespace detail { namespace { void mailman_loop(); } } }

// static helper
namespace {
struct mailman_manager
{

    typedef cppa::util::single_reader_queue<cppa::detail::mailman_job> queue_t;

    boost::thread* m_loop;
    queue_t* m_queue;

    mailman_manager()
    {
        m_queue = new queue_t;
        m_loop = new boost::thread(cppa::detail::mailman_loop);
    }

    ~mailman_manager()
    {
        m_queue->push_back(cppa::detail::mailman_job::kill_job());
        m_loop->join();
        delete m_loop;
        delete m_queue;
    }

}
s_mailman_manager;
} // namespace <anonymous>

// implementation of mailman.hpp
namespace cppa { namespace detail {

mailman_send_job::mailman_send_job(actor_proxy_ptr apptr, message msg)
    : target_peer(apptr->parent_process_ptr()), original_message(msg)
{
}

mailman_send_job::mailman_send_job(process_information_ptr peer, message msg)
    : target_peer(peer), original_message(msg)
{
}

mailman_add_peer::mailman_add_peer(native_socket_t sfd,
                                   const process_information_ptr& pinf)
    : sockfd(sfd), pinfo(pinf)
{
}

mailman_job::mailman_job(job_type jt) : next(0), m_type(jt)
{
}

mailman_job::mailman_job(process_information_ptr piptr, const message& omsg)
    : next(0), m_type(send_job_type)
{
    new (&m_send_job) mailman_send_job(piptr, omsg);
}

mailman_job::mailman_job(actor_proxy_ptr apptr, const message& omsg)
    : next(0), m_type(send_job_type)
{
    new (&m_send_job) mailman_send_job(apptr, omsg);
}

mailman_job::mailman_job(native_socket_t sockfd, const process_information_ptr& pinfo)
    : next(0), m_type(add_peer_type)
{
    new (&m_add_socket) mailman_add_peer(sockfd, pinfo);
}

mailman_job* mailman_job::kill_job()
{
    return new mailman_job(kill_type);
}

mailman_job::~mailman_job()
{
    switch (m_type)
    {
     case send_job_type:
        m_send_job.~mailman_send_job();
        break;
     case add_peer_type:
        m_add_socket.~mailman_add_peer();
        break;
    default: break;
    }
}

util::single_reader_queue<mailman_job>& mailman_queue()
{
    return *(s_mailman_manager.m_queue);
}

} } // namespace cppa::detail

namespace cppa { namespace detail { namespace {
void mailman_loop()
{
    // send() flags
    int flags = 0;
    // serializes outgoing messages
    binary_serializer bs;
    // current active job
    mailman_job* job = nullptr;
    // caches mailman_queue()
    auto& mqueue = mailman_queue();
    // connected tcp peers
    std::map<process_information, native_socket_t> peers;
    for (;;)
    {
        job = mqueue.pop();
        if (job->is_send_job())
        {
            mailman_send_job& sjob = job->send_job();
            const message& out_msg = sjob.original_message;
            // forward message to receiver peer
            auto peer_element = peers.find(*(sjob.target_peer));
            if (peer_element != peers.end())
            {
                bool disconnect_peer = false;
                auto peer = peer_element->second;
                try
                {
                    bs << out_msg;
                    auto size32 = static_cast<std::uint32_t>(bs.size());
//cout << pself.process_id << " --> " << (to_string(out_msg) + "\n");
                    // write size of serialized message
                    auto sent = ::send(peer, &size32, sizeof(size32), flags);
                    if (sent <= 0)
                    {
                        // write message
                        sent = ::send(peer, bs.data(), bs.size(), flags);
                    }
                    // disconnect peer if send() failed
                    disconnect_peer = (sent > 0);
                }
                // something went wrong; close connection to this peer
                catch (...)
                {
                    disconnect_peer = true;
                }
                if (disconnect_peer)
                {
                    closesocket(peer);
                    peers.erase(peer_element);
                }
                bs.reset();
            }
            // else: unknown peer
        }
        else if (job->is_add_peer_job())
        {
            mailman_add_peer& pjob = job->add_peer_job();
            auto i = peers.find(*(pjob.pinfo));
            if (i == peers.end())
            {
                //cout << "mailman added " << pjob.pinfo->process_id << "@"
                //     << pjob.pinfo->node_id_as_string() << endl;
                peers.insert(std::make_pair(*(pjob.pinfo), pjob.sockfd));
            }
            else
            {
                // TODO: some kind of error handling?
            }
        }
        else if (job->is_kill_job())
        {
            delete job;
            return;
        }
        delete job;
    }
}
} } } // namespace cppa::detail::<anonymous>