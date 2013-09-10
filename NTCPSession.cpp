#include <string.h>
#include <stdlib.h>
#include <endian.h>
#include <time.h>
#include <boost/bind.hpp>
#include <cryptopp/dh.h>
#include <cryptopp/secblock.h>
#include <cryptopp/dsa.h>
#include "base64.h"
#include "Log.h"
#include "CryptoConst.h"
#include "I2NPProtocol.h"
#include "RouterContext.h"
#include "Transports.h"
#include "NTCPSession.h"

using namespace i2p::crypto;

namespace i2p
{
namespace ntcp
{
	NTCPSession::NTCPSession (boost::asio::ip::tcp::socket& s, const i2p::data::RouterInfo * in_RemoteRouterInfo): 
		m_Socket (s), m_IsEstablished (false), m_ReceiveBufferOffset (0)
	{		
		if (in_RemoteRouterInfo)
			m_RemoteRouterInfo = *in_RemoteRouterInfo;
	}

	void NTCPSession::CreateAESKey (uint8_t * pubKey, uint8_t * aesKey)
	{
		CryptoPP::DH dh (elgp, elgg);
		CryptoPP::SecByteBlock secretKey(dh.AgreedValueLength());
		if (!dh.Agree (secretKey, i2p::context.GetPrivateKey (), pubKey))
		{    
		    LogPrint ("Couldn't create shared key");
			Terminate ();
			return;
		};

		if (secretKey[0] & 0x80)
		{
			aesKey[0] = 0;
			memcpy (aesKey + 1, secretKey, 31);
		}	
		else	
			memcpy (aesKey, secretKey, 32);
	}	

	void NTCPSession::Terminate ()
	{
		m_Socket.close ();
		// TODO: notify tunnels
		i2p::transports.RemoveNTCPSession (this);
	}	
		
	void NTCPSession::ClientLogin ()
	{
		// send Phase1
		const uint8_t * x = i2p::context.GetRouterIdentity ().publicKey;
		memcpy (m_Phase1.pubKey, x, 256);
		CryptoPP::SHA256().CalculateDigest(m_Phase1.HXxorHI, x, 256);
		const uint8_t * ident = m_RemoteRouterInfo.GetIdentHash ();
		for (int i = 0; i < 32; i++)
			m_Phase1.HXxorHI[i] ^= ident[i];
		
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase1, sizeof (m_Phase1)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase1Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	

	void NTCPSession::ServerLogin ()
	{
		// receive Phase1
		boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase1, sizeof (m_Phase1)),                     
			boost::bind(&NTCPSession::HandlePhase1Received, this, 
				boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	
		
	void NTCPSession::HandlePhase1Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 1 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 1 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase2, sizeof (m_Phase2)),                  
				boost::bind(&NTCPSession::HandlePhase2Received, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		}	
	}	

	void NTCPSession::HandlePhase1Received (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Phase 1 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 1 received: ", bytes_transferred);
			// verify ident
			uint8_t digest[32];
			CryptoPP::SHA256().CalculateDigest(digest, m_Phase1.pubKey, 256);
			const uint8_t * ident = i2p::context.GetRouterInfo ().GetIdentHash ();
			for (int i = 0; i < 32; i++)
			{	
				if ((m_Phase1.HXxorHI[i] ^ ident[i]) != digest[i])
				{
					LogPrint ("Wrong ident");
					Terminate ();
					return;
				}	
			}	
			
			SendPhase2 ();
		}	
	}	

	void NTCPSession::SendPhase2 ()
	{
		const uint8_t * y = i2p::context.GetRouterIdentity ().publicKey;
		memcpy (m_Phase2.pubKey, y, 256);
		uint8_t xy[512];
		memcpy (xy, m_Phase1.pubKey, 256);
		memcpy (xy + 256, y, 256);
		CryptoPP::SHA256().CalculateDigest(m_Phase2.encrypted.hxy, xy, 512); 
		uint32_t tsB = htobe32 (time(0));
		m_Phase2.encrypted.timestamp = tsB;
		// TODO: fill filler

		uint8_t aesKey[32];
		CreateAESKey (m_Phase1.pubKey, aesKey);
		m_Encryption.SetKeyWithIV (aesKey, 32, y + 240);
		m_Decryption.SetKeyWithIV (aesKey, 32, m_Phase1.HXxorHI + 16);
		
		m_Encryption.ProcessData((uint8_t *)&m_Phase2.encrypted, (uint8_t *)&m_Phase2.encrypted, sizeof(m_Phase2.encrypted));
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase2, sizeof (m_Phase2)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase2Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsB));

	}	
		
	void NTCPSession::HandlePhase2Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 2 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 2 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase3, sizeof (m_Phase3)),                   
				boost::bind(&NTCPSession::HandlePhase3Received, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsB));
		}	
	}	
		
	void NTCPSession::HandlePhase2Received (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Phase 2 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 2 received: ", bytes_transferred);
		
			uint8_t aesKey[32];
			CreateAESKey (m_Phase2.pubKey, aesKey);
			m_Decryption.SetKeyWithIV (aesKey, 32, m_Phase2.pubKey + 240);
			m_Encryption.SetKeyWithIV (aesKey, 32, m_Phase1.HXxorHI + 16);
			
			m_Decryption.ProcessData((uint8_t *)&m_Phase2.encrypted, (uint8_t *)&m_Phase2.encrypted, sizeof(m_Phase2.encrypted));
			// verify
			uint8_t xy[512], hxy[32];
			memcpy (xy, i2p::context.GetRouterIdentity ().publicKey, 256);
			memcpy (xy + 256, m_Phase2.pubKey, 256);
			CryptoPP::SHA256().CalculateDigest(hxy, xy, 512); 
			if (memcmp (hxy, m_Phase2.encrypted.hxy, 32))
			{
				LogPrint ("Incorrect hash");
				Terminate ();
				return ;
			}	
			SendPhase3 ();
		}	
	}	

	void NTCPSession::SendPhase3 ()
	{
		m_Phase3.size = htons (sizeof (m_Phase3.ident));
		memcpy (&m_Phase3.ident, &i2p::context.GetRouterIdentity (), sizeof (m_Phase3.ident));		
		uint32_t tsA = htobe32 (time(0));
		m_Phase3.timestamp = tsA;
		
		SignedData s;
		memcpy (s.x, m_Phase1.pubKey, 256);
		memcpy (s.y, m_Phase2.pubKey, 256);
		memcpy (s.ident, m_RemoteRouterInfo.GetIdentHash (), 32);
		s.tsA = tsA;
		s.tsB = m_Phase2.encrypted.timestamp;
		i2p::context.Sign ((uint8_t *)&s, sizeof (s), m_Phase3.signature);

		m_Encryption.ProcessData((uint8_t *)&m_Phase3, (uint8_t *)&m_Phase3, sizeof(m_Phase3));
		        
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase3, sizeof (m_Phase3)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase3Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsA));				
	}	
		
	void NTCPSession::HandlePhase3Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsA)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 3 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 3 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Phase4, sizeof (m_Phase4)),                  
				boost::bind(&NTCPSession::HandlePhase4Received, this, 
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, tsA));
		}	
	}	

	void NTCPSession::HandlePhase3Received (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB)
	{	
		if (ecode)
        {
			LogPrint ("Phase 3 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 3 received: ", bytes_transferred);
			m_Decryption.ProcessData((uint8_t *)&m_Phase3, (uint8_t *)&m_Phase3, sizeof(m_Phase3));
			m_RemoteRouterInfo.SetRouterIdentity (m_Phase3.ident);

			SignedData s;
			memcpy (s.x, m_Phase1.pubKey, 256);
			memcpy (s.y, m_Phase2.pubKey, 256);
			memcpy (s.ident, i2p::context.GetRouterInfo ().GetIdentHash (), 32);
			s.tsA = m_Phase3.timestamp;
			s.tsB = tsB;
			
			CryptoPP::DSA::PublicKey pubKey;
			pubKey.Initialize (dsap, dsaq, dsag, CryptoPP::Integer (m_RemoteRouterInfo.GetRouterIdentity ().signingKey, 128));
			CryptoPP::DSA::Verifier verifier (pubKey);
			if (!verifier.VerifyMessage ((uint8_t *)&s, sizeof(s), m_Phase3.signature, 40))
			{	
				LogPrint ("signature verification failed");
				Terminate ();
				return;
			}	

			SendPhase4 (tsB);
		}	
	}

	void NTCPSession::SendPhase4 (uint32_t tsB)
	{
		SignedData s;
		memcpy (s.x, m_Phase1.pubKey, 256);
		memcpy (s.y, m_Phase2.pubKey, 256);
		memcpy (s.ident, m_RemoteRouterInfo.GetIdentHash (), 32);
		s.tsA = m_Phase3.timestamp;
		s.tsB = tsB;
		i2p::context.Sign ((uint8_t *)&s, sizeof (s), m_Phase4.signature);
		m_Encryption.ProcessData((uint8_t *)&m_Phase4, (uint8_t *)&m_Phase4, sizeof(m_Phase4));

		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Phase4, sizeof (m_Phase4)), boost::asio::transfer_all (),
        	boost::bind(&NTCPSession::HandlePhase4Sent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	

	void NTCPSession::HandlePhase4Sent (const boost::system::error_code& ecode,  std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Couldn't send Phase 4 message: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 4 sent: ", bytes_transferred);
			m_IsEstablished = true;
			m_ReceiveBufferOffset = 0;
			m_DecryptedBufferOffset = 0;
			Receive ();
		}	
	}	
		
	void NTCPSession::HandlePhase4Received (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsA)
	{
		if (ecode)
        {
			LogPrint ("Phase 4 read error: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Phase 4 received: ", bytes_transferred);
			m_Decryption.ProcessData((uint8_t *)&m_Phase4, (uint8_t *)&m_Phase4, sizeof(m_Phase4));

			// verify signature
			SignedData s;
			memcpy (s.x, m_Phase1.pubKey, 256);
			memcpy (s.y, m_Phase2.pubKey, 256);
			memcpy (s.ident, i2p::context.GetRouterInfo ().GetIdentHash (), 32);
			s.tsA = tsA;
			s.tsB = m_Phase2.encrypted.timestamp;

			CryptoPP::DSA::PublicKey pubKey;
			pubKey.Initialize (dsap, dsaq, dsag, CryptoPP::Integer (m_RemoteRouterInfo.GetRouterIdentity ().signingKey, 128));
			CryptoPP::DSA::Verifier verifier (pubKey);
			if (!verifier.VerifyMessage ((uint8_t *)&s, sizeof(s), m_Phase4.signature, 40))
			{	
				LogPrint ("signature verification failed");
				Terminate ();
				return;
			}	
			m_IsEstablished = true;
			
			SendTimeSyncMessage ();

			uint8_t buf1[1000];
			int l = CreateDatabaseStoreMsg (buf1, 1000);
			SendMessage (buf1, l);

			l = CreateDeliveryStatusMsg (buf1, 1000);
			SendMessage (buf1, l);			
			
			m_ReceiveBufferOffset = 0;
			m_DecryptedBufferOffset = 0;
			Receive ();
		}
	}

	void NTCPSession::Receive ()
	{
		m_Socket.async_read_some (boost::asio::buffer(m_ReceiveBuffer + m_ReceiveBufferOffset, NTCP_MAX_MESSAGE_SIZE*2 -m_ReceiveBufferOffset),                
			boost::bind(&NTCPSession::HandleReceived, this, 
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}	
		
	void NTCPSession::HandleReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Read error: ", ecode.message ());
			Terminate ();
		}
		else
		{
			LogPrint ("Received: ", bytes_transferred);
			m_ReceiveBufferOffset += bytes_transferred;
			div_t d = div (m_ReceiveBufferOffset, 16);
			if (d.quot)
			{	
				int decryptedLen = d.quot*16;
				DecryptReceived (m_ReceiveBuffer, decryptedLen);
				if (d.rem)
				{	
					// we guarantee no overlap due if (d.quot)
					memcpy (m_ReceiveBuffer, m_ReceiveBuffer + decryptedLen, d.rem);
					m_ReceiveBufferOffset = d.rem;
				}	
				else
					m_ReceiveBufferOffset = 0;
			}	
			Receive ();
		}	
	}	

	void NTCPSession::DecryptReceived (uint8_t * encrypted, int len)
	{
		// here we might want to pass it to another thread
		m_Decryption.ProcessData(m_DecryptedBuffer + m_DecryptedBufferOffset, encrypted, len);
		m_DecryptedBufferOffset += len;
		
		
		int size = m_DecryptedBufferOffset;
		uint8_t * buf = m_DecryptedBuffer;

		while (size > 2)
		{	
			uint16_t dataSize = be16toh (*(uint16_t *)buf);
			int len = dataSize ? dataSize + 6 : 16; // 0 mean timestamp and size = 16
			if (dataSize) // regular message
			{	
				int rem = len % 16;
				if (rem > 0) len += 16 - rem;
			}	
			if (len > size) break;
			HandleNextMessage (buf, len, dataSize);
			buf += len; size -= len;
		}

		if (buf != m_DecryptedBuffer)
		{
			if (size > 0)
				memmove (m_DecryptedBuffer, buf, size);
			m_DecryptedBufferOffset = size;
		}
	}	
		
	void NTCPSession::HandleNextMessage (uint8_t * buf, int len, int dataSize)
	{
		if (dataSize)
			i2p::HandleI2NPMessage (*this, buf+2, dataSize);
		else	
			LogPrint ("Timestamp");	
	}	

	void NTCPSession::Send (const uint8_t * buf, int len, bool zeroSize)
	{
		*((uint16_t *)m_SendBuffer) = zeroSize ? 0 :htobe16 (len);
		int rem = (len + 6) % 16;
		int padding = 0;
		if (rem > 0) padding = 16 - rem;
		memcpy (m_SendBuffer + 2, buf, len);
		// TODO: fill padding 
		m_Adler.CalculateDigest (m_SendBuffer + len + 2 + padding, m_SendBuffer, len + 2+ padding);

		int l = len + padding + 6;
		m_Encryption.ProcessData(m_SendBuffer, m_SendBuffer, l);

		boost::asio::async_write (m_Socket, boost::asio::buffer (m_SendBuffer, l), boost::asio::transfer_all (),                      
        	boost::bind(&NTCPSession::HandleSent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));				
	}
		
	void NTCPSession::HandleSent (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{		
		
		if (ecode)
        {
			LogPrint ("Couldn't send msg: ", ecode.message ());
			Terminate ();
		}
		else
		{	
			LogPrint ("Msg sent: ", bytes_transferred);
		}	
	}

	void NTCPSession::SendTimeSyncMessage ()
	{
		uint32_t t = htobe32 (time (0));
		Send ((uint8_t *)&t, 4, true);
	}	

	void NTCPSession::SendMessage (uint8_t * buf, int len)
	{
		Send (buf, len);
	}
		
	NTCPClient::NTCPClient (boost::asio::io_service& service, const char * address, 
		int port, const i2p::data::RouterInfo& in_RouterInfo): NTCPSession (m_Socket, &in_RouterInfo),
		m_Socket (service), m_Endpoint (boost::asio::ip::address::from_string (address), port)	
	{
		Connect ();
	}

	void NTCPClient::Connect ()
	{
		 m_Socket.async_connect (m_Endpoint, boost::bind (&NTCPClient::HandleConnect,
			this, boost::asio::placeholders::error));
	}	

	void NTCPClient::HandleConnect (const boost::system::error_code& ecode)
	{
		if (ecode)
        {
			LogPrint ("Connect error: ", ecode.message ());
			Terminate ();
		}
		else
		{
			LogPrint ("Connected");
			ClientLogin ();
		}	
	}	

	NTCPServerConnection::NTCPServerConnection (boost::asio::io_service& service):
		NTCPSession (m_Socket), m_Socket (service)
	{
	}	
}	
}	
