// Copyright (c) 2023. ByteDance Inc. All rights reserved.


#pragma once

#include <cstdint>
#include <chrono>
#include "utils/thirdparty/quiche/rtt_stats.h"
#include "basefw/base/log.h"
#include "utils/rttstats.h"
#include "utils/transporttime.h"
#include "utils/defaultclock.hpp"
#include "sessionstreamcontroller.hpp"
#include "packettype.h"

enum class CongestionCtlType : uint8_t
{
    none = 0,
    reno = 1,
    cubic = 2
};

struct LossEvent
{
    bool valid{ false };
    // There may be multiple timeout events at one time
    std::vector<InflightPacket> lossPackets;
    Timepoint losttic{ Timepoint::Infinite() };

    std::string DebugInfo() const
    {
        std::stringstream ss;
        ss << "valid: " << valid << " "
           << "lossPackets:{";
        for (const auto& pkt: lossPackets)
        {
            ss << pkt;
        }

        ss << "} "
           << "losttic: " << losttic.ToDebuggingValue() << " ";
        return ss.str();
    }
};

struct AckEvent
{
    /** since we receive packets one by one, each packet carries only one data piece*/
    bool valid{ false };
    DataPacket ackPacket;
    Timepoint sendtic{ Timepoint::Infinite() };
    Timepoint losttic{ Timepoint::Infinite() };

    std::string DebugInfo() const
    {
        std::stringstream ss;
        ss << "valid: " << valid << " "
           << "ackpkt:{"
           << "seq: " << ackPacket.seq << " "
           << "dataid: " << ackPacket.pieceId << " "
           << "} "
           << "sendtic: " << sendtic.ToDebuggingValue() << " "
           << "losttic: " << losttic.ToDebuggingValue() << " ";
        return ss.str();
    }
};

/** This is a loss detection algorithm interface
 *  similar to the GeneralLossAlgorithm interface in Quiche project
 * */
class LossDetectionAlgo
{

public:
    /** @brief this function will be called when loss detection may happen, like timer alarmed or packet acked
     * input
     * @param downloadingmap all the packets inflight, sent but not acked or lost
     * @param eventtime timpoint that this function is called
     * @param ackEvent  ack event that trigger this function, if any
     * @param maxacked max sequence number that has acked
     * @param rttStats RTT statics module
     * output
     * @param losses loss event
     * */
    virtual void DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime,
            const AckEvent& ackEvent, uint64_t maxacked, LossEvent& losses, RttStats& rttStats)
    {
    };

    virtual ~LossDetectionAlgo() = default;

};

class DefaultLossDetectionAlgo : public LossDetectionAlgo
{/// Check loss event based on RTO
public:
    void DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime, const AckEvent& ackEvent,
            uint64_t maxacked, LossEvent& losses, RttStats& rttStats) override
    {
        SPDLOG_TRACE("inflight: {} eventtime: {} ackEvent:{} ", downloadingmap.DebugInfo(),
                eventtime.ToDebuggingValue(), ackEvent.DebugInfo());
        /** RFC 9002 Section 6
         * */
        Duration maxrtt = std::max(rttStats.previous_srtt(), rttStats.latest_rtt());
        if (maxrtt == Duration::Zero())
        {
            SPDLOG_DEBUG(" {}", maxrtt == Duration::Zero());
            maxrtt = rttStats.SmoothedOrInitialRtt();
        }
        Duration loss_delay = maxrtt + (maxrtt * (5.0 / 4.0));
        loss_delay = std::max(loss_delay, Duration::FromMicroseconds(1));
        SPDLOG_TRACE(" maxrtt: {}, loss_delay: {}", maxrtt.ToDebuggingValue(), loss_delay.ToDebuggingValue());
        for (const auto& pkt_itor: downloadingmap.inflightPktMap)
        {
            const auto& pkt = pkt_itor.second;
            if (Timepoint(pkt.sendtic + loss_delay) <= eventtime)
            {
                losses.lossPackets.emplace_back(pkt);
            }
        }
        if (!losses.lossPackets.empty())
        {
            losses.losttic = eventtime;
            losses.valid = true;
            SPDLOG_DEBUG("losses: {}", losses.DebugInfo());
        }
    }

    ~DefaultLossDetectionAlgo() override
    {
    }

private:
};


class CongestionCtlAlgo
{
public:

    virtual ~CongestionCtlAlgo() = default;

    virtual CongestionCtlType GetCCtype() = 0;

    /////  Event
    virtual void OnDataSent(const InflightPacket& sentpkt) = 0;

    virtual void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) = 0;

    /////
    virtual uint32_t GetCWND() = 0;

//    virtual uint32_t GetFreeCWND() = 0;

};


/// config or setting for specific cc algo
/// used for pass parameters to CongestionCtlAlgo
struct CongestionCtlConfig
{
    uint32_t minCwnd{ 1 };
    uint32_t maxCwnd{ 64 };
    uint32_t ssThresh{ 32 };/** slow start threshold*/
};

class RenoCongestionContrl : public CongestionCtlAlgo
{
public:

    explicit RenoCongestionContrl(const CongestionCtlConfig& ccConfig)
    {
        m_ssThresh = ccConfig.ssThresh;
        m_minCwnd = ccConfig.minCwnd;
        m_maxCwnd = ccConfig.maxCwnd;
        SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
    }

    ~RenoCongestionContrl() override
    {
        SPDLOG_DEBUG("");
    }

    CongestionCtlType GetCCtype() override
    {
        return CongestionCtlType::reno;
    }

    void OnDataSent(const InflightPacket& sentpkt) override
    {
        SPDLOG_TRACE("");
    }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    {
        SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
        if (lossEvent.valid)
        {
            OnDataLoss(lossEvent);
        }

        if (ackEvent.valid)
        {
            OnDataRecv(ackEvent, rttstats);
        }

    }

    /////
    uint32_t GetCWND() override
    {
        SPDLOG_TRACE(" {}", m_cwnd);
        return m_cwnd;
    }

//    virtual uint32_t GetFreeCWND() = 0;

private:

    bool InSlowStart()
    {
        bool rt = false;
        if (m_cwnd < m_ssThresh)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
        return rt;
    }

    bool LostCheckRecovery(Timepoint largestLostSentTic)
    {
        SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
                largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
        /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
         * (plus a 10ms correction), this session is in Recovery phase.
         * */
        if (lastLagestLossPktSentTic.IsInitialized() &&
            (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
        {
            SPDLOG_DEBUG("In Recovery");
            return true;
        }
        else
        {
            // a new timelost
            lastLagestLossPktSentTic = largestLostSentTic;
            SPDLOG_DEBUG("new loss");
            return false;
        }

    }

    void ExitSlowStart()
    {
        SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
        m_ssThresh = m_cwnd;
    }


    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    {
        // SPDLOG_INFO("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(), m_cwnd);
        SPDLOG_INFO("rtt:{}", rttstats.latest_rtt().ToDebuggingValue());
        if (InSlowStart())
        {
            /// add 1 for each ack event
            m_cwnd += 1;

            if (m_cwnd >= m_ssThresh)
            {
                ExitSlowStart();
            }
            SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
        }
        else
        {
            /// add cwnd for each RTT
            m_cwndCnt++;
            m_cwnd += m_cwndCnt / m_cwnd;
            if (m_cwndCnt == m_cwnd)
            {
                m_cwndCnt = 0;
            }
            SPDLOG_DEBUG("not in slow start state,new m_cwndCnt:{} new m_cwnd:{}",
                    m_cwndCnt, ackEvent.DebugInfo(), m_cwnd);

        }
        m_cwnd = BoundCwnd(m_cwnd);

        SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
    }

    void OnDataLoss(const LossEvent& lossEvent)
    {
        // SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(),m_cwnd);
        Timepoint maxsentTic{ Timepoint::Zero() };

        for (const auto& lostpkt: lossEvent.lossPackets)
        {
            maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
        }

        /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
         *  Otherwise, cwnd will cut half.
         * */
        if (InSlowStart())
        {
            // loss in slow start, just cut half
            m_cwnd = m_cwnd / 2;
            m_cwnd = BoundCwnd(m_cwnd);

        }
        else //if (!LostCheckRecovery(maxsentTic))
        {
            // Not In slow start and not inside Recovery state
            // Cut half
            m_cwnd = m_cwnd / 2;
            m_cwnd = BoundCwnd(m_cwnd);
            m_ssThresh = m_cwnd;
            // enter Recovery state
        }
        SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
    }


    uint32_t BoundCwnd(uint32_t trySetCwnd)
    {
        return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    }

    uint32_t m_cwnd{ 1 };
    uint32_t m_cwndCnt{ 0 }; /** in congestion avoid phase, used for counting ack packets*/
    Timepoint lastLagestLossPktSentTic{ Timepoint::Zero() };


    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 64 };
    uint32_t m_ssThresh{ 32 };/** slow start threshold*/
};



// struct CubicCongestionCtlConfig
// {
//     bool fast_convergence{ 0 };
//     bool tcp_friendliness{ 0 };
//     uint32_t minCwnd{ 1 };
//     uint32_t maxCwnd{ 64 };
//     uint32_t ssThresh{ 32 };/** slow start threshold*/
// };

class CubicCongestionContrl : public CongestionCtlAlgo
{
public:

    explicit CubicCongestionContrl(const CongestionCtlConfig& ccConfig)
    {
        // m_fast_convergence = ccConfig.fast_convergence;
        // m_tcp_friendliness = ccConfig.tcp_friendliness;
        m_ssThresh = ccConfig.ssThresh;
        m_minCwnd = ccConfig.minCwnd;
        m_maxCwnd = ccConfig.maxCwnd;
        SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
    }

    ~CubicCongestionContrl() override
    {
        SPDLOG_DEBUG("");
    }

    CongestionCtlType GetCCtype() override
    {
        return CongestionCtlType::cubic;
    }

    void OnDataSent(const InflightPacket& sentpkt) override
    {
        SPDLOG_TRACE("");
    }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    {
        SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
        if (lossEvent.valid)
        {
            OnDataLoss(lossEvent);
        }

        if (ackEvent.valid)
        {
            OnDataRecv(ackEvent, rttstats);
        }

    }

    /////
    uint32_t GetCWND() override
    {
        SPDLOG_TRACE(" {}", m_cwnd);
        return m_cwnd;
    }

private:
    
    bool InSlowStart()
    {
        bool rt = false;
        if (m_cwnd < m_ssThresh)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
        return rt;
    }

    void ExitSlowStart()
    {
        SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
        m_ssThresh = m_cwnd;
    }

    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    {
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(), m_cwnd);
        SPDLOG_INFO("rtt:{}", rttstats.latest_rtt().ToDebuggingValue());
        if (InSlowStart())
        {
            /// add 1 for each ack event
            m_cwnd += 1;

            if (m_cwnd >= m_ssThresh)
            {
                ExitSlowStart();
            }
            SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
        }
        else
        {
            /// add cwnd for each RTT
            uint32_t cnt = cubic_update(ackEvent, rttstats);
            if ( m_cwnd_cnt > cnt )
            {
                m_cwnd ++;
                m_cwnd_cnt = 0;
            }
            else m_cwnd_cnt ++;
            SPDLOG_DEBUG("not in slow start state,new m_cwnd_cnt:{} new m_cwnd:{}",
                    m_cwnd_cnt, ackEvent.DebugInfo(), m_cwnd);

        }
        m_cwnd = BoundCwnd(m_cwnd);

        SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
    }

    void OnDataLoss(const LossEvent& lossEvent)
    {
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(),m_cwnd);

        epoch_start = Timepoint::Zero();
        if (m_cwnd < m_last_max_cwnd && m_fast_convergence)
            m_last_max_cwnd = m_cwnd * (2-cubic_beta)/2;
        else
            m_last_max_cwnd = m_cwnd;

        m_cwnd = m_cwnd * (1-cubic_beta);
        m_ssThresh = m_cwnd;
        
        SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
    }

    uint32_t cubic_update(const AckEvent& ackEvent, RttStats& rttstats)
    {
        uint32_t origin_point;
        if (epoch_start <= Timepoint::Zero())
        {
            epoch_start = Timepoint( ackEvent.sendtic + rttstats.latest_rtt() );
            if (m_cwnd < m_last_max_cwnd)
            {
                bic_K = Duration::FromMilliseconds( uint32_t( pow((m_last_max_cwnd-m_cwnd)/cubic_C, 1.0/3) ) );
                origin_point = m_last_max_cwnd;
            }
            else
            {
                bic_K = Duration::FromMilliseconds( 0 );
                origin_point = m_cwnd;
            }
            m_ack_cnt = 1;
            m_tcp_cwnd = m_cwnd;
        }

        Duration time_offset = Duration::Zero(); // |t-K|
        Duration rec_time = ackEvent.sendtic + rttstats.latest_rtt() + rttstats.MinOrInitialRtt() - epoch_start;

        if (rec_time < bic_K)		
            time_offset = bic_K - rec_time;
        else
            time_offset = rec_time - bic_K;
        
        uint32_t target;
        target = origin_point + cubic_C * time_offset.ToMilliseconds() * time_offset.ToMilliseconds() * time_offset.ToMilliseconds();
        
        uint32_t cnt;
        if (target > m_cwnd)
            cnt = m_cwnd / ( target - m_cwnd );
        else 
            cnt = 100 * m_cwnd;
        
        if (m_tcp_friendliness)
        {    
            m_tcp_cwnd += 3*cubic_beta/(2-cubic_beta) * m_ack_cnt/m_cwnd;
            m_ack_cnt = 0;
            if (m_tcp_cwnd > m_cwnd)
            {
                uint32_t max_cnt = m_cwnd / ( m_tcp_cwnd - m_cwnd );
                if (cnt > max_cnt) cnt = max_cnt; 
            }
        }
        return cnt;
    }


    uint32_t BoundCwnd(uint32_t trySetCwnd)
    {
        return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    }

    uint32_t m_cwnd{ 1 };
    uint32_t m_tcp_cwnd{ 1 };
    uint32_t m_ack_cnt{ 0 }; /* used in tcp friendliness*/
    uint32_t m_cwnd_cnt{ 0 }; /* in congestion avoid phase, used for counting ack packets*/
    uint32_t m_last_max_cwnd{ 0 };
    Timepoint lastLagestLossPktSentTic{ Timepoint::Zero() };

    Timepoint epoch_start{ Timepoint::Zero() };
    Duration bic_K{ Duration::Zero() };
    double cubic_C{ 0.4 * 1000};
    double cubic_beta{ 0.2 };
    
    bool m_tcp_friendliness{ 0 };
    bool m_fast_convergence{ 0 };
    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 64 };
    uint32_t m_ssThresh{ 32 };/** slow start threshold*/
};
