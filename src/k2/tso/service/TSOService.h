/*
MIT License

Copyright(c) 2020 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#pragma once
#include <chrono>
#include <climits>
#include <tuple>

// third-party
#include <seastar/core/distributed.hh>  // for distributed<>
#include <seastar/core/future.hh>       // for future stuff

#include <k2/appbase/Appbase.h>
#include <k2/common/Chrono.h>
#include <k2/dto/MessageVerbs.h>
#include <k2/dto/TimestampBatch.h>

namespace k2
{
using namespace dto;

// TSO (controller) internal API verbs to Paxos for heart beat etc. and to Atomic/GPS clock for accurate time
enum TSOInternalVerbs : k2::Verb {
    GET_PAXOS_LEADER_URL    = 110,  // API from TSO controller to any Paxos instance to get leader instance URL
    UPDATE_PAXOS            = 111,  // API from TSO controller to Paxos leader to send heart beat(conditional write with read) and other updates(compete for master, etc)
    ACK_PAXOS               = 112,  // ACK from PAXOS to TSO
    GET_ATOMIC_CLOCK_TIME   = 115,  // API from TSO controller to its atomic clock to get current time
    GET_GPS_CLOCK_TIME      = 116,  // API from TSO client to get timestamp batch from any TSO worker cores
    ACK_TIME                = 117   // ACK to TSO client for above APIs
};

// TSOService is reponsible to provide batches of K2 TimeStamps to TSO client upon request.
class TSOService
{
public: // types

    // the control infor from Controller setting to all workers
    // all ticks are in nanoseconds
   struct TSOWorkerControlInfo
    {
        bool        IsReadyToIssueTS;       // if this core is allowed to issue TS, could be false for various reasons (TODO: consider adding reasons)
        uint8_t     TBENanoSecStep;         // step to skip between timestamp in nanoSec, actually same as the number of worker cores
        uint64_t    TBEAdjustment;       // batch ending time adjustment from current chrono::system_clock::now(), in nanoSec;
        uint16_t    TsDelta;                // batch starting time adjustment from TbeTSEAdjustment, basically the uncertainty window size, in nanoSec
        uint64_t    ReservedTimeShreshold;  // reservedTimeShreshold upper bound, the generated batch and TS in it can't be bigger than that, in nanoSec counts
        uint16_t    BatchTTL;               // TTL of batch issued in nanoseconds, not expected to change once set

        TSOWorkerControlInfo() : IsReadyToIssueTS(false), TBENanoSecStep(0), TBEAdjustment(0), TsDelta(0), ReservedTimeShreshold(0), BatchTTL(0) {};
    };

    // TODO: worker/controller statistics structure typedef

public :  // application lifespan
    TSOService();
    ~TSOService();

    // required for seastar::distributed interface
    seastar::future<> gracefulStop();
    seastar::future<> start();

    //TODO: implement this
    uint32_t TSOId() {return 1;};

    // worker public APIs
    // worker API updating the controlInfo, triggered from controller through SS cross-core communication
    void UpdateWorkerControlInfo(const TSOWorkerControlInfo& controlInfo);

    // get worker endpoint URLs of all transport stack, TCP/IP, RDMA, etc.
    std::vector<k2::String> GetWorkerURLs();

    // controller public APIs

private:
    // types

    // forward delclaration of controller and worker roles
    // Each core in TSOServerice takes one role, core 0 take controller role and the rest of cores takes worker roles.
    // worker core is responsible to take requests from TSO client to issue timestamp(batch)
    // controller core is responsible to manage this process instance(participate master election, etc),
    // periodically sync up with atomic/GPS clock and update reserved timeshreshold, etc.
    class TSOController;
    class TSOWorker;

    // these two roles do not exist on one core at the same time
    std::unique_ptr<TSOController> _controller;
    std::unique_ptr<TSOWorker> _worker;

};  // class TSOService

// TSOController - core 0 of a TSO server, all other cores are TSOWorkers.
// responsible to handle following four things
// 1. Upon start, join the cluster and get the for instance (role of instance can change upon API SetRole() as well)
// 2. Upon role change, set or adjust heartbeat - If master role, heartbeat also extends lease and extends ReservedTimeShreshold. If standby role, hearbeat check master's lease/healthness.
//    In master role, if ReservedTimeShreshold get extended, update TSOWorkerControlInfo to all workers.
// 3. Periodically checkAtomicGPSClock, and adjust TBEAdjustment if needed and further more update TSOWorkerControlInfo to all workers upon such adjustment. Note: If not master role, doing this for optimization.
// 4. If master role, periodically collect statistics from all worker cores and reports.
class TSOService::TSOController
{
    public:
    TSOController(TSOService& outer) :
        _outer(outer),
        _heartBeatTimer([this]{this->HeartBeat();}),
        _timeSyncTimer([this]{this->TimeSync();}),
        _statsUpdateTimer([this]{this->CollectAndReportStats();}){};

    // start the controller
    // Assumption: caller will wait the start() fully complete
    // Internally, it will
    // 1) InitializeInternal, including init control info, gather worker URLs, sync time with atomic clock;
    // 2) then join the cluster;
    // 3) then set role (master or standby)
    // 4) then arm timers and register public RPC APIs
    seastar::future<> start();

    // stop the controller
    // Internally, it will
    // 1) set stop requested(maybe already done)
    // 2) then unregister public RPC APIs
    // 3) then wait for all three timered task done and cancel timers respectively
    // 4) then exit cluster
    // NOTE: stop may need one full cycle of heartbeat() to finish, default 10ms.
    seastar::future<> gracefulStop();

    DISABLE_COPY_MOVE(TSOController);

    private:

    // Design Note:
    // 1. Interaction between controller and workers
    //    a) during start(), controller collect workers URLs
    //       and if controller after JoinServerCluster() find itself is master,
    //       it will UpdateWorkerControlInfo() through out of band DoHeartBeat() to enable workers start serving requests
    //    b) Once started, controller will only UpdateWorkerControlInfo() through regular HeartBeat()
    // 2. Internally inside controller
    //    a) during start(), initialize controller JoinServerCluster() and if master, update workers through out of band DoHeartBeat()
    //    b) Once started, only periodically HeartBeat() will handle all complex logic including role change, updating worker, handle gracefully stop()/lost lease suicide().
    //    c) TimeSyn() only update in memory _controlInfoToSend, which will be sent with next HeartBeat();

    // First step of Initialize controller before JoinServerCluster() during start()
    seastar::future<> InitializeInternal();

    // initialize TSOWorkerControlInfo at start()
    inline void InitWorkerControlInfo();

    seastar::future<> GetAllWorkerURLs();

    // Join the TSO server cluster during start().
    // return tuple
    //  - element 0 - if this instance is a master or not.
    //  - element 1 - prevReservedTimeShreshold if this instance is mater, the value need to be waited out by this master instnace to avoid duplicate timestamp.
    // TODO: implement this
    seastar::future<std::tuple<bool, uint64_t>> JoinServerCluster()
    {
        K2INFO("JoinServerCluster");
        // fake new master
        std::tuple<bool, uint64_t> result(true, 0);
        _myLease = GenNewLeaseVal();
        _masterInstanceURL = k2::RPC().getServerEndpoint(k2::TCPRPCProtocol::proto)->getURL();
        return seastar::make_ready_future<std::tuple<bool, uint64_t>>(result);
    }

    // APIs registration
    // APIs to TSO clients
    void RegisterGetTSOMasterURL();
    void RegisterGetTSOWorkersURLs();
    // internal API responses Paxos and Atomic/GPS clock();
    void RegisterACKPaxos() { return; }
    void RegisterACKTime() { return; }

    // TODO: implement this
    seastar::future<> ExitServerCluster()
    {
        return seastar::make_ready_future<>();
    }

    // Change my role inside the controller, from Master to StandBy(isMaster passed in is true) or from Standby to Master
    // SetRoleInternal will, if needed, trigger out of band heartbeat and worker control info update, to prepare workers in ready mode
    // Note: Asssumption - When this is called, Paxos already had properly updated master related record. This assumption is also the reason this fn is called "Internal" (of the controller object)
    //       This fn is called during start() after JoinServerCluster, and during regular HeartBeat() which could find out role change.
    seastar::future<> SetRoleInternal(bool isMaster, uint64_t prevReservedTimeShreshold);

    // periodically send heart beat and handle heartbeat response
    // If this is Master Instance, heart beat will renew the lease, extend the ReservedTimeShreshold if needed
    // If this is Standby Instance, heart beat will maintain the membership, and check Master Instance status and take master role if needed
    void HeartBeat();

    // helper to do the HeartBeat(), could be called from regular HeartBeat(), or during initialization or inside HearBeat() when role need to be changed
    seastar::future<> DoHeartBeat();

    // helper for DoHeartBeat() when _stopRequested
    seastar::future<> DoHeartBeatDuringStop();

    // this is lambda set in HeartBeat() to handle the response,
    // For standby instance, may
    seastar::future<> HandleHeartBeatResponse() { return seastar::make_ready_future<>(); }

    // TimeSync timer call back fn.
    void TimeSync();
    // helper function which do the real work of time sync.
    seastar::future<> DoTimeSync();

    // check atomic/GPS clock and return an effective uncertainty windows of time containing current real time
    // return value is actually two unint64 values<T, V>, the first one is the difference of TAI TSE(in nanosec) since Jan. 1, 1970 to local steady_clock, the second value is uncertainty window size(in nanosec)
    // The current time (uncertainty window) will be <steady_clock::now() + T - V/2, steady_clock::now() + T + V/2>
    seastar::future<std::tuple<uint64_t, uint64_t>> CheckAtomicGPSClock();

    // Once we have updated controlInfo due to any reason, e.g. role change, ReservedTimeShreshold or drift from atomic clock,
    // propagate the update to all workers and
    // The control into to send is at member _controlInfoToSend, except IsReadyToIssueTS, which will be set inside this fn based on the current state
    seastar::future<> SendWorkersControlInfo();

    // periodically collect stats from workers and report
    void CollectAndReportStats();
    seastar::future<> DoCollectAndReportStats();


    // suicide when and only when we are master and find we lost lease
    void Suicide();


    // helpers to talk to Paxos
    // TODO: Consider role change,
    // TODO: implement this
    seastar::future<> RemoveLeaseFromPaxos() {return seastar::make_ready_future<>();}
    seastar::future<> RemoveLeaseFromPaxosWithUpdatingReservedTimeShreshold(/*uint64_t newReservedTimsShreshold*/) {return seastar::make_ready_future<>();}
    seastar::future<uint64_t> RenewLeaseOnly() {return seastar::make_ready_future<uint64_t>(GenNewLeaseVal());}

    // regular heartbeat update to Paxos when not a master
    seastar::future<> UpdateStandByHeartBeat() {return seastar::make_ready_future<>();}

    // regular heartbeat update to Paxos when is a master
    // return future contains newly extended Lease and ReservedTimeThreshold in nanosec count
    seastar::future<std::tuple<uint64_t, uint64_t>>RenewLeaseAndExtendReservedTimeThreshold()
    {

        auto extendedLeaseAndThreshold = GenNewLeaseVal();
        std::tuple<uint64_t, uint64_t> tup(extendedLeaseAndThreshold, extendedLeaseAndThreshold);
        return seastar::make_ready_future<std::tuple<uint64_t, uint64_t>>(tup);
    }

    // (in nanosec counts) Current TA time + three times of heartBeat + 1 extra millisecond to allow missing up to 3 heartbeat before loose leases
    inline uint64_t GenNewLeaseVal() { return TimeAuthorityNow() + _heartBeatTimerInterval().count() * 3 + 1*1000*1000;}

    // outer TSOService object
    TSOService& _outer;

    // _isMasterInstance, set when join cluster or with heartbeat
    bool _isMasterInstance{false};

    // URL of current TSO master instance
    k2::String _masterInstanceURL;

    // worker cores' URLs, each worker can have mulitple urls
    std::vector<std::vector<k2::String>> _workersURLs;

    // The difference between the TA(Time Authority) and local time (local steady clock as it is strictly increasing).
    // This is part of TBEAdjustment. This is kept to detect local steady_clock drift away from Time Authority at each TimeSyncTask.
    uint64_t _diffTALocalInNanosec {0};

    // known current time of TA(TimeAuthority), local steady_clock time now + the diff between, in units of nanosec since Jan. 1, 1970 (TAI)
    inline uint64_t TimeAuthorityNow() {return now_nsec_count() +  _diffTALocalInNanosec; }

    // when this instance become (new) master, it need to get previous master's ReservedTimeShreshold
    // and wait out this time if current time is less than this value
    uint64_t _prevReservedTimeShreshold{ULLONG_MAX};

    // Lease at the Paxos, whem this is master, updated by heartbeat.
    uint64_t _myLease;

    // set when stop() is called
    bool _stopRequested{false};

    // last sent (to workers) worker control info
    TSOWorkerControlInfo _lastSentControlInfo;
    // current control info that is updated and to be sent to worker
    // Note: IsReadyToIssueTS is only set inside SendWorkersControlInfo() based on the state when SendWorkersControlInfo() is called
    TSOWorkerControlInfo _controlInfoToSend;

    seastar::timer<> _heartBeatTimer;
    ConfigDuration _heartBeatTimerInterval{"tso.ctrol_heart_beat_interval", 10ms};
    seastar::future<> _heartBeatFuture = seastar::make_ready_future<>();  // need to keep track of heartbeat task future for proper shutdown

    seastar::timer<> _timeSyncTimer;
    ConfigDuration _timeSyncTimerInterval{"tso.ctrol_time_sync_interval", 10ms};
    seastar::future<> _timeSyncFuture = seastar::make_ready_future<>();  // need to keep track of timeSync task future for proper shutdown

    // this is the batch uncertainty windows size, should be less than MTL(minimal transaction latency), 
    // this is also used at the TSO client side as batch's TTL(Time To Live)
    // TODO: consider derive this value from MTL configuration.
    ConfigDuration _defaultTBWindowSize{"tso.ctrol_ts_batch_win_size", 8ms}; 

    seastar::timer<> _statsUpdateTimer;
    ConfigDuration _statsUpdateTimerInterval{"tso.ctrol_stats_update_interval", 1s};
    seastar::future<> _statsUpdateFuture = seastar::make_ready_future<>();  // need to keep track of statsUpdate task future for proper shutdown

};

// TSOWorker - worker cores of TSO service that take TSO client requests and issue Timestamp (batch).
// responsible to handle following three things, if this TSO is master instance role.
// 1. handle TSO client request, issuing time stamp (batch). This is a normal priority task.
// 2. handle config data(TSOWorkerControlInfo below) update task issued from the control core. This is a high priority task.
// 3. collect and aggregate statistics data of this core for control core to collect. This is a low priority task.
class TSOService::TSOWorker
{
    public:
    TSOWorker(TSOService& outer) : _outer(outer){};

    seastar::future<> gracefulStop();
    seastar::future<> start();

    DISABLE_COPY_MOVE(TSOWorker);

    // get updated controlInfo from controller and update local copy
    void UpdateWorkerControlInfo(const TSOWorkerControlInfo& controlInfo);

    // periodical task to send statistics to controller core
    seastar::future<> SendWorkderStatistics() {return seastar::make_ready_future<>();};

    private:
    // outer TSOService object
    TSOService& _outer;
    uint32_t _tsoId;    // keep a copy to avoid access _outer in TS issuing hot path

    // current worker control info
    TSOWorkerControlInfo _curControlInfo;

    // last request's TBE(Timestamp Batch End) time rounded at microsecond level 
    uint64_t _lastRequestTBEMicroSecRounded{0};  
    // count of timestamp issued in last request's timestamp batch 
    // Note: each worker core can issue up to (1000/TBENanoSecStep) timestamps within same microsecond (at TBE)
    uint16_t _lastRequestTimeStampCount{0};

    // TODO: statistics structure

    // APIs to TSO clients
    void RegisterGetTSOTimestampBatch();

    // the main API for TSO client to get timestamp in batch
    // batchSizeRequested may be partically fulfilled based on server side timestamp availability
    TimestampBatch GetTimestampFromTSO(uint16_t batchSizeRequested);
    // helper function to issue timestamp (or check error situation)
    TimestampBatch GetTimeStampFromTSOLessFrequentHelper(uint16_t batchSizeRequested, uint64_t nowMicroSecRounded);

    // private helper
    // helpers for updateWorkerControlInfo
    void AdjustWorker(const TSOWorkerControlInfo& controlInfo);


};

// TSO service should be started with at least two cores, one controller and rest are workers.
class TSONotEnoughCoreException : public std::exception {
public:
    TSONotEnoughCoreException(uint16_t coreCount) : _coreCount(coreCount) {};

private:
    virtual const char* what() const noexcept override { return "TSONotEnoughCoreException: Need at least two cores. core counts" + _coreCount; }

    uint16_t _coreCount {0};
};

// TSO server not ready yet to issue timestamp(batch)
// TODO: add more detail error info.
class TSONotReadyException : public std::exception {
    private:
    virtual const char* what() const noexcept override { return "Server not ready to issue timestamp, please retry later."; }
};

// operations invalid during server shutdown
class TSOShutdownException : public std::exception {
    private:
    virtual const char* what() const noexcept override { return "TSO Server shuts down."; }
};

} // namespace k2
