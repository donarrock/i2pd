#include <inttypes.h>
#include <openssl/sha.h>
#include "I2PEndian.h"
#include <map>
#include <string>
#include "Crypto.h"
#include "Elligator.h"
#include "RouterContext.h"
#include "I2NPProtocol.h"
#include "Tunnel.h"
#include "TunnelPool.h"
#include "Transports.h"
#include "Timestamp.h"
#include "Log.h"
#include "FS.h"
#include "Garlic.h"

namespace i2p
{
namespace garlic
{
	GarlicRoutingSession::GarlicRoutingSession (GarlicDestination * owner,
	    std::shared_ptr<const i2p::data::RoutingDestination> destination, int numTags, bool attachLeaseSet):
		m_Owner (owner), m_Destination (destination), m_NumTags (numTags),
		m_LeaseSetUpdateStatus (attachLeaseSet ? eLeaseSetUpdated : eLeaseSetDoNotSend),
		m_LeaseSetUpdateMsgID (0)
	{
		// create new session tags and session key
		RAND_bytes (m_SessionKey, 32);
		m_Encryption.SetKey (m_SessionKey);
	}

	GarlicRoutingSession::GarlicRoutingSession (const uint8_t * sessionKey, const SessionTag& sessionTag):
		m_Owner (nullptr), m_NumTags (1), m_LeaseSetUpdateStatus (eLeaseSetDoNotSend), m_LeaseSetUpdateMsgID (0)
	{
		memcpy (m_SessionKey, sessionKey, 32);
		m_Encryption.SetKey (m_SessionKey);
		m_SessionTags.push_back (sessionTag);
		m_SessionTags.back ().creationTime = i2p::util::GetSecondsSinceEpoch ();
	}

	GarlicRoutingSession::~GarlicRoutingSession	()
	{
	}

	std::shared_ptr<GarlicRoutingPath> GarlicRoutingSession::GetSharedRoutingPath ()
	{
		if (!m_SharedRoutingPath) return nullptr;
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		if (m_SharedRoutingPath->numTimesUsed >= ROUTING_PATH_MAX_NUM_TIMES_USED ||
		    !m_SharedRoutingPath->outboundTunnel->IsEstablished () ||
			ts*1000LL > m_SharedRoutingPath->remoteLease->endDate ||
		    ts > m_SharedRoutingPath->updateTime + ROUTING_PATH_EXPIRATION_TIMEOUT)
				m_SharedRoutingPath = nullptr;
		if (m_SharedRoutingPath) m_SharedRoutingPath->numTimesUsed++;
		return m_SharedRoutingPath;
	}

	void GarlicRoutingSession::SetSharedRoutingPath (std::shared_ptr<GarlicRoutingPath> path)
	{
		if (path && path->outboundTunnel && path->remoteLease)
		{
			path->updateTime = i2p::util::GetSecondsSinceEpoch ();
			path->numTimesUsed = 0;
		}
		else
			path = nullptr;
		m_SharedRoutingPath = path;
	}

	GarlicRoutingSession::UnconfirmedTags * GarlicRoutingSession::GenerateSessionTags ()
	{
		auto tags = new UnconfirmedTags (m_NumTags);
		tags->tagsCreationTime = i2p::util::GetSecondsSinceEpoch ();
		for (int i = 0; i < m_NumTags; i++)
		{
			RAND_bytes (tags->sessionTags[i], 32);
			tags->sessionTags[i].creationTime = tags->tagsCreationTime;
		}
		return tags;
	}

	void GarlicRoutingSession::MessageConfirmed (uint32_t msgID)
	{
		TagsConfirmed (msgID);
		if (msgID == m_LeaseSetUpdateMsgID)
		{
			m_LeaseSetUpdateStatus = eLeaseSetUpToDate;
			m_LeaseSetUpdateMsgID = 0;
			LogPrint (eLogInfo, "Garlic: LeaseSet update confirmed");
		}
		else
			CleanupExpiredTags ();
	}

	void GarlicRoutingSession::TagsConfirmed (uint32_t msgID)
	{
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		auto it = m_UnconfirmedTagsMsgs.find (msgID);
		if (it != m_UnconfirmedTagsMsgs.end ())
		{
			auto& tags = it->second;
			if (ts < tags->tagsCreationTime + OUTGOING_TAGS_EXPIRATION_TIMEOUT)
			{
				for (int i = 0; i < tags->numTags; i++)
					m_SessionTags.push_back (tags->sessionTags[i]);
			}
			m_UnconfirmedTagsMsgs.erase (it);
		}
	}

	bool GarlicRoutingSession::CleanupExpiredTags ()
	{
		auto ts = i2p::util::GetSecondsSinceEpoch ();
		for (auto it = m_SessionTags.begin (); it != m_SessionTags.end ();)
		{
			if (ts >= it->creationTime + OUTGOING_TAGS_EXPIRATION_TIMEOUT)
				it = m_SessionTags.erase (it);
			else
				++it;
		}
		CleanupUnconfirmedTags ();
		if (m_LeaseSetUpdateMsgID && ts*1000LL > m_LeaseSetSubmissionTime + LEASET_CONFIRMATION_TIMEOUT)
		{
			if (m_Owner)
				m_Owner->RemoveDeliveryStatusSession (m_LeaseSetUpdateMsgID);
			m_LeaseSetUpdateMsgID = 0;
		}
		return !m_SessionTags.empty () || !m_UnconfirmedTagsMsgs.empty ();
	}

	bool GarlicRoutingSession::CleanupUnconfirmedTags ()
	{
		bool ret = false;
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		// delete expired unconfirmed tags
		for (auto it = m_UnconfirmedTagsMsgs.begin (); it != m_UnconfirmedTagsMsgs.end ();)
		{
			if (ts >= it->second->tagsCreationTime + OUTGOING_TAGS_CONFIRMATION_TIMEOUT)
			{
				if (m_Owner)
					m_Owner->RemoveDeliveryStatusSession (it->first);
				it = m_UnconfirmedTagsMsgs.erase (it);
				ret = true;
			}
			else
				++it;
		}
		return ret;
	}

	std::shared_ptr<I2NPMessage> GarlicRoutingSession::WrapSingleMessage (std::shared_ptr<const I2NPMessage> msg)
	{
		auto m = NewI2NPMessage ();
		m->Align (12); // in order to get buf aligned to 16 (12 + 4)
		size_t len = 0;
		uint8_t * buf = m->GetPayload () + 4; // 4 bytes for length

		// find non-expired tag
		bool tagFound = false;
		SessionTag tag;
		if (m_NumTags > 0)
		{
			uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
			while (!m_SessionTags.empty ())
			{
				if (ts < m_SessionTags.front ().creationTime + OUTGOING_TAGS_EXPIRATION_TIMEOUT)
				{
					tag = m_SessionTags.front ();
					m_SessionTags.pop_front (); // use same tag only once
					tagFound = true;
					break;
				}
				else
					m_SessionTags.pop_front (); // remove expired tag
			}
		}
		// create message
		if (!tagFound) // new session
		{
			LogPrint (eLogInfo, "Garlic: No tags available, will use ElGamal");
			if (!m_Destination)
			{
				LogPrint (eLogError, "Garlic: Can't use ElGamal for unknown destination");
				return nullptr;
			}
			// create ElGamal block
			ElGamalBlock elGamal;
			memcpy (elGamal.sessionKey, m_SessionKey, 32);
			RAND_bytes (elGamal.preIV, 32); // Pre-IV
			uint8_t iv[32]; // IV is first 16 bytes
			SHA256(elGamal.preIV, 32, iv);
			BN_CTX * ctx = BN_CTX_new ();
			m_Destination->Encrypt ((uint8_t *)&elGamal, buf, ctx);
			BN_CTX_free (ctx);
			m_Encryption.SetIV (iv);
			buf += 514;
			len += 514;
		}
		else // existing session
		{
			// session tag
			memcpy (buf, tag, 32);
			uint8_t iv[32]; // IV is first 16 bytes
			SHA256(tag, 32, iv);
			m_Encryption.SetIV (iv);
			buf += 32;
			len += 32;
		}
		// AES block
		len += CreateAESBlock (buf, msg);
		htobe32buf (m->GetPayload (), len);
		m->len += len + 4;
		m->FillI2NPMessageHeader (eI2NPGarlic);
		return m;
	}

	size_t GarlicRoutingSession::CreateAESBlock (uint8_t * buf, std::shared_ptr<const I2NPMessage> msg)
	{
		size_t blockSize = 0;
		bool createNewTags = m_Owner && m_NumTags && ((int)m_SessionTags.size () <= m_NumTags*2/3);
		UnconfirmedTags * newTags = createNewTags ? GenerateSessionTags () : nullptr;
		htobuf16 (buf, newTags ? htobe16 (newTags->numTags) : 0); // tag count
		blockSize += 2;
		if (newTags) // session tags recreated
		{
			for (int i = 0; i < newTags->numTags; i++)
			{
				memcpy (buf + blockSize, newTags->sessionTags[i], 32); // tags
				blockSize += 32;
			}
		}
		uint32_t * payloadSize = (uint32_t *)(buf + blockSize);
		blockSize += 4;
		uint8_t * payloadHash = buf + blockSize;
		blockSize += 32;
		buf[blockSize] = 0; // flag
		blockSize++;
		size_t len = CreateGarlicPayload (buf + blockSize, msg, newTags);
		htobe32buf (payloadSize, len);
		SHA256(buf + blockSize, len, payloadHash);
		blockSize += len;
		size_t rem = blockSize % 16;
		if (rem)
			blockSize += (16-rem); //padding
		m_Encryption.Encrypt(buf, blockSize, buf);
		return blockSize;
	}

	size_t GarlicRoutingSession::CreateGarlicPayload (uint8_t * payload, std::shared_ptr<const I2NPMessage> msg, UnconfirmedTags * newTags)
	{
		uint64_t ts = i2p::util::GetMillisecondsSinceEpoch ();
		uint32_t msgID;
		RAND_bytes ((uint8_t *)&msgID, 4);
		size_t size = 0;
		uint8_t * numCloves = payload + size;
		*numCloves = 0;
		size++;

		if (m_Owner)
		{
			// resubmit non-confirmed LeaseSet
			if (m_LeaseSetUpdateStatus == eLeaseSetSubmitted && ts > m_LeaseSetSubmissionTime + LEASET_CONFIRMATION_TIMEOUT)
			{
				m_LeaseSetUpdateStatus = eLeaseSetUpdated;
				SetSharedRoutingPath (nullptr); // invalidate path since leaseset was not confirmed
			}

			// attach DeviveryStatus if necessary
			if (newTags || m_LeaseSetUpdateStatus == eLeaseSetUpdated) // new tags created or leaseset updated
			{
				// clove is DeliveryStatus
				auto cloveSize = CreateDeliveryStatusClove (payload + size, msgID);
				if (cloveSize > 0) // successive?
				{
					size += cloveSize;
					(*numCloves)++;
					if (newTags) // new tags created
					{
						newTags->msgID = msgID;
						m_UnconfirmedTagsMsgs.insert (std::make_pair(msgID, std::unique_ptr<UnconfirmedTags>(newTags)));
						newTags = nullptr; // got acquired
					}
					m_Owner->DeliveryStatusSent (shared_from_this (), msgID);
				}
				else
					LogPrint (eLogWarning, "Garlic: DeliveryStatus clove was not created");
			}
			// attach LeaseSet
			if (m_LeaseSetUpdateStatus == eLeaseSetUpdated)
			{
				if (m_LeaseSetUpdateMsgID) m_Owner->RemoveDeliveryStatusSession (m_LeaseSetUpdateMsgID); // remove previous
				m_LeaseSetUpdateStatus = eLeaseSetSubmitted;
				m_LeaseSetUpdateMsgID = msgID;
				m_LeaseSetSubmissionTime = ts;
				// clove if our leaseSet must be attached
				auto leaseSet = CreateDatabaseStoreMsg (m_Owner->GetLeaseSet ());
				size += CreateGarlicClove (payload + size, leaseSet, false);
				(*numCloves)++;
			}
		}
		if (msg) // clove message ifself if presented
		{
			size += CreateGarlicClove (payload + size, msg, m_Destination ? m_Destination->IsDestination () : false);
			(*numCloves)++;
		}
		memset (payload + size, 0, 3); // certificate of message
		size += 3;
		htobe32buf (payload + size, msgID); // MessageID
		size += 4;
		htobe64buf (payload + size, ts + 8000); // Expiration of message, 8 sec
		size += 8;

		if (newTags) delete newTags; // not acquired, delete
		return size;
	}

	size_t GarlicRoutingSession::CreateGarlicClove (uint8_t * buf, std::shared_ptr<const I2NPMessage> msg, bool isDestination)
	{
		uint64_t ts = i2p::util::GetMillisecondsSinceEpoch () + 8000; // 8 sec
		size_t size = 0;
		if (isDestination)
		{
			buf[size] = eGarlicDeliveryTypeDestination << 5;//  delivery instructions flag destination
			size++;
			memcpy (buf + size, m_Destination->GetIdentHash (), 32);
			size += 32;
		}
		else
		{
			buf[size] = 0;//  delivery instructions flag local
			size++;
		}

		memcpy (buf + size, msg->GetBuffer (), msg->GetLength ());
		size += msg->GetLength ();
		uint32_t cloveID;
		RAND_bytes ((uint8_t *)&cloveID, 4);
		htobe32buf (buf + size, cloveID); // CloveID
		size += 4;
		htobe64buf (buf + size, ts); // Expiration of clove
		size += 8;
		memset (buf + size, 0, 3); // certificate of clove
		size += 3;
		return size;
	}

	size_t GarlicRoutingSession::CreateDeliveryStatusClove (uint8_t * buf, uint32_t msgID)
	{
		size_t size = 0;
		if (m_Owner)
		{
			auto inboundTunnel = m_Owner->GetTunnelPool ()->GetNextInboundTunnel ();
			if (inboundTunnel)
			{
				buf[size] = eGarlicDeliveryTypeTunnel << 5; // delivery instructions flag tunnel
				size++;
				// hash and tunnelID sequence is reversed for Garlic
				memcpy (buf + size, inboundTunnel->GetNextIdentHash (), 32); // To Hash
				size += 32;
				htobe32buf (buf + size, inboundTunnel->GetNextTunnelID ()); // tunnelID
				size += 4;
				// create msg
				auto msg = CreateDeliveryStatusMsg (msgID);
				if (m_Owner)
				{
					//encrypt
					uint8_t key[32], tag[32];
					RAND_bytes (key, 32); // random session key
					RAND_bytes (tag, 32); // random session tag
					m_Owner->SubmitSessionKey (key, tag);
					GarlicRoutingSession garlic (key, tag);
					msg = garlic.WrapSingleMessage (msg);
				}
				memcpy (buf + size, msg->GetBuffer (), msg->GetLength ());
				size += msg->GetLength ();
				// fill clove
				uint64_t ts = i2p::util::GetMillisecondsSinceEpoch () + 8000; // 8 sec
				uint32_t cloveID;
				RAND_bytes ((uint8_t *)&cloveID, 4);
				htobe32buf (buf + size, cloveID); // CloveID
				size += 4;
				htobe64buf (buf + size, ts); // Expiration of clove
				size += 8;
				memset (buf + size, 0, 3); // certificate of clove
				size += 3;
			}
			else
				LogPrint (eLogError, "Garlic: No inbound tunnels in the pool for DeliveryStatus");
		}
		else
			LogPrint (eLogWarning, "Garlic: Missing local LeaseSet");

		return size;
	}

	GarlicDestination::GarlicDestination (): m_NumTags (32) // 32 tags by default
	{
		m_Ctx = BN_CTX_new ();
	}

	GarlicDestination::~GarlicDestination ()
	{
		BN_CTX_free (m_Ctx);
	}

	void GarlicDestination::CleanUp ()
	{
		m_Sessions.clear ();
		m_DeliveryStatusSessions.clear ();
		m_Tags.clear ();
	}
	void GarlicDestination::AddSessionKey (const uint8_t * key, const uint8_t * tag)
	{
		if (key)
		{
			uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
			m_Tags[SessionTag(tag, ts)] = std::make_shared<AESDecryption>(key);
		}
	}

	bool GarlicDestination::SubmitSessionKey (const uint8_t * key, const uint8_t * tag)
	{
		AddSessionKey (key, tag);
		return true;
	}

	void GarlicDestination::HandleGarlicMessage (std::shared_ptr<I2NPMessage> msg)
	{
		uint8_t * buf = msg->GetPayload ();
		uint32_t length = bufbe32toh (buf);
		if (length > msg->GetLength ())
		{
			LogPrint (eLogWarning, "Garlic: message length ", length, " exceeds I2NP message length ", msg->GetLength ());
			return;
		}
		buf += 4; // length
		auto it = m_Tags.find (SessionTag(buf));
		// AES tag might be used even if encryption type is not ElGamal/AES
		if (it != m_Tags.end ())
		{
			// tag found. Use AES
			auto decryption = it->second;
			m_Tags.erase (it); // tag might be used only once
			if (length >= 32)
			{
				uint8_t iv[32]; // IV is first 16 bytes
				SHA256(buf, 32, iv);
				decryption->SetIV (iv);
				decryption->Decrypt (buf + 32, length - 32, buf + 32);
				HandleAESBlock (buf + 32, length - 32, decryption, msg->from);
			}
			else
				LogPrint (eLogWarning, "Garlic: message length ", length, " is less than 32 bytes");
		}
		else
		{
			// tag not found. Handle depending on encryption type
			if (GetEncryptionType () == i2p::data::CRYPTO_KEY_TYPE_ECIES_X25519_AEAD_RARCHET)
			{
				HandleECIESx25519 (buf, length - 4);	
				return;
			}	
			// otherwise assume ElGamal/AES	
			ElGamalBlock elGamal;
			if (length >= 514 && Decrypt (buf, (uint8_t *)&elGamal, m_Ctx))
			{
				auto decryption = std::make_shared<AESDecryption>(elGamal.sessionKey);
				uint8_t iv[32]; // IV is first 16 bytes
				SHA256(elGamal.preIV, 32, iv);
				decryption->SetIV (iv);
				decryption->Decrypt(buf + 514, length - 514, buf + 514);
				HandleAESBlock (buf + 514, length - 514, decryption, msg->from);
			}
			else
				LogPrint (eLogError, "Garlic: Failed to decrypt message");
		}
	}

	void GarlicDestination::HandleAESBlock (uint8_t * buf, size_t len, std::shared_ptr<AESDecryption> decryption,
		std::shared_ptr<i2p::tunnel::InboundTunnel> from)
	{
		uint16_t tagCount = bufbe16toh (buf);
		buf += 2; len -= 2;
		if (tagCount > 0)
		{
			if (tagCount*32 > len)
			{
				LogPrint (eLogError, "Garlic: Tag count ", tagCount, " exceeds length ", len);
				return ;
			}
			uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
			for (int i = 0; i < tagCount; i++)
				m_Tags[SessionTag(buf + i*32, ts)] = decryption;
		}
		buf += tagCount*32;
		len -= tagCount*32;
		uint32_t payloadSize = bufbe32toh (buf);
		if (payloadSize > len)
		{
			LogPrint (eLogError, "Garlic: Unexpected payload size ", payloadSize);
			return;
		}
		buf += 4;
		uint8_t * payloadHash = buf;
		buf += 32;// payload hash.
		if (*buf) // session key?
			buf += 32; // new session key
		buf++; // flag

		// payload
		uint8_t digest[32];
		SHA256 (buf, payloadSize, digest);
		if (memcmp (payloadHash, digest, 32)) // payload hash doesn't match
		{
			LogPrint (eLogError, "Garlic: wrong payload hash");
			return;
		}
		HandleGarlicPayload (buf, payloadSize, from);
	}

	void GarlicDestination::HandleGarlicPayload (uint8_t * buf, size_t len, std::shared_ptr<i2p::tunnel::InboundTunnel> from)
	{
		if (len < 1)
		{
			LogPrint (eLogError, "Garlic: payload is too short");
			return;
		}
		int numCloves = buf[0];
		LogPrint (eLogDebug, "Garlic: ", numCloves," cloves");
		buf++; len--;
		for (int i = 0; i < numCloves; i++)
		{
			const uint8_t * buf1 = buf;
			// delivery instructions
			uint8_t flag = buf[0];
			buf++; // flag
			if (flag & 0x80) // encrypted?
			{
				// TODO: implement
				LogPrint (eLogWarning, "Garlic: clove encrypted");
				buf += 32;
			}
			ptrdiff_t offset = buf - buf1;
			GarlicDeliveryType deliveryType = (GarlicDeliveryType)((flag >> 5) & 0x03);
			switch (deliveryType)
			{
				case eGarlicDeliveryTypeLocal:
					LogPrint (eLogDebug, "Garlic: type local");
					if (offset > (int)len)
					{
						LogPrint (eLogError, "Garlic: message is too short");
						break;
					}
					HandleI2NPMessage (buf, len - offset);
				break;
				case eGarlicDeliveryTypeDestination:
					LogPrint (eLogDebug, "Garlic: type destination");
					buf += 32; // destination. check it later or for multiple destinations
					offset = buf - buf1;
					if (offset > (int)len)
					{
						LogPrint (eLogError, "Garlic: message is too short");
						break;
					}
					HandleI2NPMessage (buf, len - offset);
				break;
				case eGarlicDeliveryTypeTunnel:
				{
					LogPrint (eLogDebug, "Garlic: type tunnel");
					// gwHash and gwTunnel sequence is reverted
					uint8_t * gwHash = buf;
					buf += 32;
					offset = buf - buf1;
					if (offset + 4 > (int)len)
					{
						LogPrint (eLogError, "Garlic: message is too short");
						break;
					}
					uint32_t gwTunnel = bufbe32toh (buf);
					buf += 4; offset += 4;
					auto msg = CreateI2NPMessage (buf, GetI2NPMessageLength (buf, len - offset), from);
					if (from) // received through an inbound tunnel
					{
						std::shared_ptr<i2p::tunnel::OutboundTunnel> tunnel;
						if (from->GetTunnelPool ())
							tunnel = from->GetTunnelPool ()->GetNextOutboundTunnel ();
						else
							LogPrint (eLogError, "Garlic: Tunnel pool is not set for inbound tunnel");
						if (tunnel) // we have sent it through an outbound tunnel
							tunnel->SendTunnelDataMsg (gwHash, gwTunnel, msg);
						else
							LogPrint (eLogWarning, "Garlic: No outbound tunnels available for garlic clove");
					}
					else // received directly
						i2p::transport::transports.SendMessage (gwHash, i2p::CreateTunnelGatewayMsg (gwTunnel, msg)); // send directly
					break;
				}
				case eGarlicDeliveryTypeRouter:
				{
					uint8_t * ident = buf;
					buf += 32;
					offset = buf - buf1;
					if (!from) // received directly
					{
						if (offset > (int)len)
						{
							LogPrint (eLogError, "Garlic: message is too short");
							break;
						}
						i2p::transport::transports.SendMessage (ident,
							CreateI2NPMessage (buf, GetI2NPMessageLength (buf, len - offset)));
					}
					else
						LogPrint (eLogWarning, "Garlic: type router for inbound tunnels not supported");
					break;
				}
				default:
					LogPrint (eLogWarning, "Garlic: unknown delivery type ", (int)deliveryType);
			}
			if (offset > (int)len)
			{
				LogPrint (eLogError, "Garlic: message is too short");
				break;
			}
			buf += GetI2NPMessageLength (buf, len - offset); //  I2NP
			buf += 4; // CloveID
			buf += 8; // Date
			buf += 3; // Certificate
			offset = buf - buf1;
			if (offset > (int)len)
			{
				LogPrint (eLogError, "Garlic: clove is too long");
				break;
			}
			len -= offset;
		}
	}

	std::shared_ptr<I2NPMessage> GarlicDestination::WrapMessage (std::shared_ptr<const i2p::data::RoutingDestination> destination,
		std::shared_ptr<I2NPMessage> msg, bool attachLeaseSet)
	{
		auto session = GetRoutingSession (destination, attachLeaseSet);
		return session->WrapSingleMessage (msg);
	}

	std::shared_ptr<GarlicRoutingSession> GarlicDestination::GetRoutingSession (
		std::shared_ptr<const i2p::data::RoutingDestination> destination, bool attachLeaseSet)
	{
		GarlicRoutingSessionPtr session;
		{
			std::unique_lock<std::mutex> l(m_SessionsMutex);
			auto it = m_Sessions.find (destination->GetIdentHash ());
			if (it != m_Sessions.end ())
				session = it->second;
		}
		if (!session)
		{
			session = std::make_shared<GarlicRoutingSession> (this, destination,
				attachLeaseSet ? m_NumTags : 4, attachLeaseSet); // specified num tags for connections and 4 for LS requests
			std::unique_lock<std::mutex> l(m_SessionsMutex);
			m_Sessions[destination->GetIdentHash ()] = session;
		}
		return session;
	}

	void GarlicDestination::CleanupExpiredTags ()
	{
		// incoming
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		int numExpiredTags = 0;
		for (auto it = m_Tags.begin (); it != m_Tags.end ();)
		{
			if (ts > it->first.creationTime + INCOMING_TAGS_EXPIRATION_TIMEOUT)
			{
				numExpiredTags++;
				it = m_Tags.erase (it);
			}
			else
				++it;
		}
		if (numExpiredTags > 0)
			LogPrint (eLogDebug, "Garlic: ", numExpiredTags, " tags expired for ", GetIdentHash().ToBase64 ());

		// outgoing
		{
			std::unique_lock<std::mutex> l(m_SessionsMutex);
			for (auto it = m_Sessions.begin (); it != m_Sessions.end ();)
			{
				it->second->GetSharedRoutingPath (); // delete shared path if necessary
				if (!it->second->CleanupExpiredTags ())
				{
					LogPrint (eLogInfo, "Routing session to ", it->first.ToBase32 (), " deleted");
					it->second->SetOwner (nullptr);
					it = m_Sessions.erase (it);
				}
				else
					++it;
			}
		}
		// delivery status sessions
		{
			std::unique_lock<std::mutex> l(m_DeliveryStatusSessionsMutex);
			for (auto it = m_DeliveryStatusSessions.begin (); it != m_DeliveryStatusSessions.end (); )
			{
				if (it->second->GetOwner () != this)
					it = m_DeliveryStatusSessions.erase (it);
				else
					++it;
			}
		}
	}

	void GarlicDestination::RemoveDeliveryStatusSession (uint32_t msgID)
	{
		std::unique_lock<std::mutex> l(m_DeliveryStatusSessionsMutex);
		m_DeliveryStatusSessions.erase (msgID);
	}

	void GarlicDestination::DeliveryStatusSent (GarlicRoutingSessionPtr session, uint32_t msgID)
	{
		std::unique_lock<std::mutex> l(m_DeliveryStatusSessionsMutex);
		m_DeliveryStatusSessions[msgID] = session;
	}

	void GarlicDestination::HandleDeliveryStatusMessage (uint32_t msgID)
	{
		GarlicRoutingSessionPtr session;
		{
			std::unique_lock<std::mutex> l(m_DeliveryStatusSessionsMutex);
			auto it = m_DeliveryStatusSessions.find (msgID);
			if (it != m_DeliveryStatusSessions.end ())
			{
				session = it->second;
				m_DeliveryStatusSessions.erase (it);
			}
		}
		if (session)
		{
			session->MessageConfirmed (msgID);
			LogPrint (eLogDebug, "Garlic: message ", msgID, " acknowledged");
		}
	}

	void GarlicDestination::SetLeaseSetUpdated ()
	{
		std::unique_lock<std::mutex> l(m_SessionsMutex);
		for (auto& it: m_Sessions)
			it.second->SetLeaseSetUpdated ();
	}

	void GarlicDestination::ProcessGarlicMessage (std::shared_ptr<I2NPMessage> msg)
	{
		HandleGarlicMessage (msg);
	}

	void GarlicDestination::ProcessDeliveryStatusMessage (std::shared_ptr<I2NPMessage> msg)
	{
		uint32_t msgID = bufbe32toh (msg->GetPayload () + DELIVERY_STATUS_MSGID_OFFSET);
		HandleDeliveryStatusMessage (msgID);
	}

	void GarlicDestination::SaveTags ()
	{
		if (m_Tags.empty ()) return;
		std::string ident = GetIdentHash().ToBase32();
		std::string path  = i2p::fs::DataDirPath("tags", (ident + ".tags"));
		std::ofstream f (path, std::ofstream::binary | std::ofstream::out | std::ofstream::trunc);
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		// 4 bytes timestamp, 32 bytes tag, 32 bytes key
		for (auto it: m_Tags)
		{
			if (ts < it.first.creationTime + INCOMING_TAGS_EXPIRATION_TIMEOUT)
			{
				f.write ((char *)&it.first.creationTime, 4);
				f.write ((char *)it.first.data (), 32);
				f.write ((char *)it.second->GetKey ().data (), 32);
			}
		}
	}

	void GarlicDestination::LoadTags ()
	{
		std::string ident = GetIdentHash().ToBase32();
		std::string path  = i2p::fs::DataDirPath("tags", (ident + ".tags"));
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		if (ts < i2p::fs::GetLastUpdateTime (path) + INCOMING_TAGS_EXPIRATION_TIMEOUT)
		{
			// might contain non-expired tags
			std::ifstream f (path, std::ifstream::binary);
			if (f)
			{
				std::map<i2p::crypto::AESKey, std::shared_ptr<AESDecryption> > keys;
				// 4 bytes timestamp, 32 bytes tag, 32 bytes key
				while (!f.eof ())
				{
					uint32_t t;
					uint8_t tag[32], key[32];
					f.read ((char *)&t, 4); if (f.eof ()) break;
					if (ts < t + INCOMING_TAGS_EXPIRATION_TIMEOUT)
					{
						f.read ((char *)tag, 32);
						f.read ((char *)key, 32);
					}
					else
						f.seekg (64, std::ios::cur); // skip
					if (f.eof ()) break;

					std::shared_ptr<AESDecryption> decryption;
					auto it = keys.find (key);
					if (it != keys.end ())
						decryption = it->second;
					else
						decryption = std::make_shared<AESDecryption>(key);
					m_Tags.insert (std::make_pair (SessionTag (tag, ts), decryption));
				}
				if (!m_Tags.empty ())
					LogPrint (eLogInfo, m_Tags.size (), " loaded for ", ident);
			}
		}
		i2p::fs::Remove (path);
	}

	void CleanUpTagsFiles ()
	{
		std::vector<std::string> files;
		i2p::fs::ReadDir (i2p::fs::DataDirPath("tags"), files);
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		for (auto  it: files)
			if (ts >= i2p::fs::GetLastUpdateTime (it) + INCOMING_TAGS_EXPIRATION_TIMEOUT)
				 i2p::fs::Remove (it);
	}

	void GarlicDestination::HandleECIESx25519 (const uint8_t * buf, size_t len)
	{
		// KDF1
		// TODO : use precalculated hashes
		static const char protocolName[41] = "Noise_IKelg2+hs2_25519_ChaChaPoly_SHA256"; // 40 bytes
		uint8_t h[64], ck[32];
		SHA256 ((const uint8_t *)protocolName, 40, h);
		memcpy (ck, h, 32);
		SHA256 (h, 32, h);				
		// we are Bob
		memcpy (h + 32, GetEncryptionPublicKey (), 32);
		SHA256 (h, 64, h); // h = SHA256(h || bpk) 
		
		uint8_t aepk[32];
		if (!i2p::crypto::GetElligator ()->Decode (buf, aepk))
		{ 
			LogPrint (eLogError, "Garlic: Can't decode elligator");
			return;	
		}
		buf += 32; len -= 32;
		memcpy (h + 32, aepk, 32);
		SHA256 (h, 64, h); // h = SHA256(h || aepk)

		uint8_t sharedSecret[32], keyData[64];
		Decrypt (aepk, sharedSecret, m_Ctx); // x25519
		i2p::crypto::HKDF (ck, sharedSecret, 32, "", keyData); // keydata = HKDF(chainKey, sharedSecret, "", 64)
		memcpy (ck, keyData, 32); // chainKey = keydata[0:31] 

		// decrypt flags/static
		uint8_t nonce[12], fs[32];
		memset (nonce, 0, 12); // n = 0
		if (!i2p::crypto::AEADChaCha20Poly1305 (buf, 32, h, 32, keyData + 32, nonce, fs, 32, false)) // decrypt
		{
			LogPrint (eLogWarning, "Garlic: Flags/static section AEAD verification failed ");
			return;
		}
		memcpy (h + 32, buf, 32);
		SHA256 (h, 64, h); // h = SHA256(h || ciphertext)
		buf += 48; len -= 48; // 32 data + 16 poly
		// decrypt payload
		std::vector<uint8_t> payload (len + 32); uint8_t h1[32]; 
		// KDF2 for payload
		bool isStatic = !i2p::data::Tag<32> (fs).IsZero (); 
		if (isStatic)
		{
			// static key, fs is apk
			Decrypt (fs, sharedSecret, m_Ctx); // DH(bsk, apk)
			i2p::crypto::HKDF (ck, sharedSecret, 32, "", keyData); // keydata = HKDF(chainKey, sharedSecret, "", 64)
			memcpy (ck, keyData, 32); // chainKey = keydata[0:31] 	
			memcpy (payload.data (), h, 32);
			memcpy (payload.data () + 32, buf, len); // h || ciphertext
			SHA256 (payload.data (), len + 32, h1); 
		}
		else // all zeros flags
			htole64buf (nonce + 4, 1); // n = 1 
		if (!i2p::crypto::AEADChaCha20Poly1305 (buf, len - 16, h, 32, keyData + 32, nonce, payload.data () + 32, len - 16, false)) // decrypt
		{
			LogPrint (eLogWarning, "Garlic: Payload section AEAD verification failed");
			return;
		}
		if (isStatic) memcpy (h, h1, 32); // h = SHA256(h || ciphertext)
		HandleECIESx25519Payload (payload.data () + 32, len - 16);
	}

	void GarlicDestination::HandleECIESx25519Payload (const uint8_t * buf, size_t len)
	{
		size_t offset = 0;
		while (offset < len)
		{
			uint8_t blk = buf[offset];
			offset++;
			auto size = bufbe16toh (buf + offset);
			offset += 2;
			LogPrint (eLogDebug, "Garlic: Block type ", (int)blk, " of size ", size);
			if (size > len)
			{
				LogPrint (eLogError, "Garlic: Unexpected block length ", size);
				break;
			}
			switch (blk)
			{
				case eECIESx25519BlkGalicClove:
					HandleECIESx25519GarlicClove (buf + offset, size);
				break;
				case eECIESx25519BlkDateTime:
					LogPrint (eLogDebug, "Garlic: datetime");
				break;	
				case eECIESx25519BlkOptions:
					LogPrint (eLogDebug, "Garlic: options");
				break;
				case eECIESx25519BlkPadding:
					LogPrint (eLogDebug, "NTCP2: padding");
				break;
				default:
					LogPrint (eLogWarning, "Garlic: Unknown block type ", (int)blk);
			}
			offset += size;
		}
	}

	void GarlicDestination::HandleECIESx25519GarlicClove (const uint8_t * buf, size_t len)
	{
		const uint8_t * buf1 = buf;	
		uint8_t flag = buf[0]; buf++; // flag
		GarlicDeliveryType deliveryType = (GarlicDeliveryType)((flag >> 5) & 0x03);
		switch (deliveryType)
		{
			case eGarlicDeliveryTypeDestination:
				buf += 32; // TODO: check destination
			// no break here
			case eGarlicDeliveryTypeLocal:
			{
				uint8_t typeID = buf[0]; buf++; // typeid
				buf += (4 + 4); // msgID + expiration
				ptrdiff_t offset = buf - buf1;
				if (offset <= (int)len)
					HandleCloveI2NPMessage (typeID, buf, len - offset);
				else
					LogPrint (eLogError, "Garlic: clove is too long");
				break;
			}
			//  TODO: tunnel
			default:
				LogPrint (eLogWarning, "Garlic: unexpected delivery type ", (int)deliveryType);
		}
	}
}
}
