#include <infinispan/hotrod/exceptions.h>
#include <infinispan/hotrod/InetSocketAddress.h>
#include <infinispan/hotrod/ServerNameId.h>
#include "hotrod/impl/transport/tcp/TcpTransportFactory.h"
#include "hotrod/impl/transport/tcp/TcpTransport.h"
#include "hotrod/impl/transport/tcp/TransportObjectFactory.h"
#include "hotrod/impl/transport/tcp/RoundRobinBalancingStrategy.h"
#include "hotrod/impl/protocol/Codec.h"
#include "infinispan/hotrod/Configuration.h"
#include "hotrod/impl/consistenthash/ConsistentHash.h"
#include "hotrod/sys/Log.h"
#include <algorithm>

namespace infinispan {
namespace hotrod {

using protocol::Codec;
using namespace sys;
using namespace consistenthash;

namespace transport {

TransportFactory* TransportFactory::newInstance(const Configuration& configuration) {
    return new TcpTransportFactory(configuration);
}

void TcpTransportFactory::start(
    Codec& codec, int defaultTopologyId)
{
	ScopedLock<Mutex> l(lock);
	topologyAge = 0;
    std::vector<ServerConfiguration> configuredServers = configuration.getServersConfiguration();
    for (std::vector<ServerConfiguration>::const_iterator iter = configuredServers.begin();
        iter != configuredServers.end(); iter++)
    {
        initialServers.push_back(InetSocketAddress(iter->getHostCString(), iter->getPort()));
    }

    FailOverRequestBalancingStrategy::ProducerFn producerFn=configuration.getBalancingStrategy();
    if (producerFn!= nullptr) {
        balancer.reset((*producerFn)());
    }
    else
    {
        balancer.reset(RoundRobinBalancingStrategy::newInstance());
    }
    topologyInfo = new TopologyInfo(defaultTopologyId, initialServers, configuration);

    transportFactory.reset(new TransportObjectFactory(codec, *this));

    createAndPreparePool();

    //  update map conversion ServerIdentity/INetSocketAddress
    std::vector<ServerNameId> serverNames;
    for (std::vector<InetSocketAddress>::const_iterator it =
            initialServers.begin(); it != initialServers.end(); ++it) {
    	const ServerNameId tmp = (ServerNameId)*it;
    	if (serverNameMap.find(tmp)==serverNameMap.end())
    	{
    		serverNameMap.insert(std::make_pair(tmp,InetSocketAddress(*it)));
    	}
    	serverNames.push_back(tmp);
    }

    balancer->setServers(serverNames);
    pingServers();
 }

Transport& TcpTransportFactory::getTransport(const std::vector<char>& /*cacheName*/) {
    const InetSocketAddress* server = NULL;
    {
        ScopedLock<Mutex> l(lock);
        std::map<ServerNameId,InetSocketAddress>::iterator it=serverNameMap.find(balancer->nextServer());
        if (it==serverNameMap.end()){
          throw Exception("Server not found!");
        }
    	server=&it->second;
    }
    return borrowTransportFromPool(*server);
}

Transport& TcpTransportFactory::getTransport(const std::vector<char>& key, const std::vector<char>& cacheName) {
    InetSocketAddress server;
    {
        ScopedLock<Mutex> l(lock);

        server = topologyInfo->getHashAwareServer(key,cacheName);
        if (server.isEmpty())
        {   // Return balanced transport
        	return getTransport(cacheName);
        }
        return borrowTransportFromPool(server);
    }
}

void TcpTransportFactory::releaseTransport(Transport& transport) {
    ConnectionPool* pool = getConnectionPool();
    TcpTransport& tcpTransport = dynamic_cast<TcpTransport&>(transport);
    if (!tcpTransport.isValid()) {
        pool->invalidateObject(tcpTransport.getServerAddress(), &tcpTransport);
    } else {
        pool->returnObject(tcpTransport.getServerAddress(), tcpTransport);
    }
}

void TcpTransportFactory::invalidateTransport(
    const InetSocketAddress& serverAddress, Transport* transport)
{
    ConnectionPool* pool = getConnectionPool();
    pool->invalidateObject(serverAddress, dynamic_cast<TcpTransport*>(transport));
}

bool TcpTransportFactory::isTcpNoDelay() {
    return configuration.isTcpNoDelay();
}

int TcpTransportFactory::getMaxRetries() {
    ScopedLock<Mutex> l(lock);
    return maxRetries;
}

int TcpTransportFactory::getSoTimeout() {
    return configuration.getSocketTimeout();
}

int TcpTransportFactory::getConnectTimeout() {
    return configuration.getConnectionTimeout();
}

bool TcpTransportFactory::isSslEnabled() {
    return configuration.getSslConfiguration().enabled();
}

const std::string& TcpTransportFactory::getSslServerCAPath() {
    return configuration.getSslConfiguration().serverCAPath();
}

const std::string& TcpTransportFactory::getSslServerCAFile() {
    return configuration.getSslConfiguration().serverCAFile();
}

const std::string& TcpTransportFactory::getSslClientCertificateFile() {
    return configuration.getSslConfiguration().clientCertificateFile();
}

void TcpTransportFactory::createAndPreparePool()
{
    connectionPool.reset(new ConnectionPool(transportFactory, configuration.getConnectionPoolConfiguration()));
    std::vector<InetSocketAddress> servers = topologyInfo->getServers();
    for (std::vector<InetSocketAddress>::const_iterator i = servers.begin();
        i != servers.end() ; ++i)
    {
        connectionPool->preparePool(*i);
    }
}

void TcpTransportFactory::pingServers() {
    std::vector<InetSocketAddress> s = topologyInfo->getServers();
    for (std::vector<InetSocketAddress>::const_iterator iter = s.begin(); iter != s.end(); iter++) {
        TcpTransport* transport;
        try {
            // Go through all statically configured nodes and force a
            // connection to be established and a ping message to be sent.
            transport = &connectionPool->borrowObject(*iter);
            transportFactory->ping(*transport);
            connectionPool->returnObject(*iter, *transport);
        } catch (const Exception &e) {
            TRACE("Initial ping has thrown an exception when pinging %s:%d : %s",
                iter->getHostname().c_str(), iter->getPort(), e.what());
            // Ping's objective is to retrieve a potentially newer
            // version of the Hot Rod cluster topology, so ignore
            // exceptions from nodes that might not be up any more.
        }
    }
}

void TcpTransportFactory::destroy() {
    ScopedLock<Mutex> l(lock);
    connectionPool->clear();
    connectionPool->close();
    delete topologyInfo;
}

Transport& TcpTransportFactory::borrowTransportFromPool(
    const InetSocketAddress& server)
{
    ConnectionPool* pool = getConnectionPool();
    return pool->borrowObject(server);
}

ConnectionPool* TcpTransportFactory::getConnectionPool()
{
    ScopedLock<Mutex> l(lock);
    return connectionPool.get();
}

void TcpTransportFactory::updateServers(std::vector<InetSocketAddress>& newServers) {
    ScopedLock<Mutex> l(lock);
    std::vector<InetSocketAddress> addedServers;
    std::vector<InetSocketAddress> topoServers=topologyInfo->getServers();

    std::sort(newServers.begin(), newServers.end());

    std::sort(topoServers.begin(), topoServers.end());

    std::set_difference(newServers.begin(), newServers.end(), topoServers.begin(),
    		topoServers.end(), std::inserter(addedServers, addedServers.end()));

    std::vector<InetSocketAddress> failedServers;
    std::sort(newServers.begin(), newServers.end());
    std::sort(topoServers.begin(), topoServers.end());

    std::set_difference(topoServers.begin(), topoServers.end(), newServers.begin(),
            newServers.end(), std::inserter(failedServers, failedServers.end()));

    if (failedServers.empty() && newServers.empty()) {
        return;
    }

    //1. first add new servers. For servers that went down, the returned transport will fail for now
    for (std::vector<InetSocketAddress>::const_iterator it =
            addedServers.begin(); it != addedServers.end(); ++it) {
        connectionPool->addObject(*it);
    }

    //2. now set the server list to the active list of servers. All the active servers (potentially together with some
    // failed servers) are in the pool now. But after this, the pool won't be asked for connections to failed servers,
    // as the balancer will only know about the active servers

    //3. update map conversion ServerIdentity/INetSocketAddress
    std::vector<ServerNameId> newServerNames;
    for (std::vector<InetSocketAddress>::const_iterator it =
            newServers.begin(); it != newServers.end(); ++it) {
    	const ServerNameId tmp = (ServerNameId)*it;
    	if (serverNameMap.find(tmp)==serverNameMap.end())
    	{
    		serverNameMap.insert(std::make_pair(tmp,InetSocketAddress(*it)));
    	}
    	newServerNames.push_back(tmp);
    }

    topologyInfo->updateServers(newServers);
    balancer->setServers(newServerNames);

    //3. Now just remove failed servers
    for (std::vector<InetSocketAddress>::const_iterator it =
            failedServers.begin(); it != failedServers.end(); ++it) {
        connectionPool->clear(*it);
    }

    topoServers.clear();
    topologyInfo->updateServers(newServers);
}

void TcpTransportFactory::updateHashFunction(
        std::map<InetSocketAddress, std::set<int32_t> >& servers2Hash,
        int32_t numKeyOwners, uint8_t hashFunctionVersion, int32_t hashSpace, const std::vector<char>& cacheName) {
    ScopedLock<Mutex> l(lock);
    topologyInfo->updateTopology(servers2Hash,numKeyOwners,hashFunctionVersion,hashSpace,cacheName);
   }

void TcpTransportFactory::updateHashFunction(
		std::vector<std::vector<InetSocketAddress>>& segmentOwners,
        uint32_t &numSegment, uint8_t &hashFunctionVersion,
        const std::vector<char>& cacheName, int topologyId)
{
    ScopedLock<Mutex> l(lock);
    TRACE("TcpTransportFactory::updateHashFunction(): hashversion=%d, topologyId=%d",hashFunctionVersion, topologyId);
   std::shared_ptr<consistenthash::ConsistentHash> h=topologyInfo->updateTopology(segmentOwners, numSegment, hashFunctionVersion, cacheName, topologyId);
}


void TcpTransportFactory::clearHashFunction(const std::vector<char>& cacheName) {
    topologyInfo->consistentHashErase(cacheName);
}

ConsistentHashFactory& TcpTransportFactory::getConsistentHashFactory(){
    ScopedLock<Mutex> l(lock);
    return *topologyInfo->getHashFactory();
}

}}} // namespace infinispan::hotrod::transport
