/*
 * Copyright (c) 2015-2021 IMDEA Networks Institute
 * Authors: Nina Grosheva <nina.grosheva@gmail.com>
 *          Hany Assasa <hany.assasa@gmail.com>
 */
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/wifi-module.h"
#include "common-functions.h"
#include <iomanip>
#include <sstream>

/**
 * Simulation Objective:
 * Evaluate SU-MIMO beamforming training and data communication in the IEEE 802.11ay standard.
 *
 * Network Topology:
 * Network topology is simple and consists of a single EDMG PCP/AP and a one EDMG STA.
 *
 * Simulation Description:
 * Both EDMG PCP/AP and EDMG STA use a parametric codebook generated by our IEEE 802.11ay
 * Codebook Generator Application in MATLAB. Each device uses either 2/3/4 antenna arrays of 2x8 Elements.
 * The channel model is generated by our Q-D realization software.
 *
 * Running the Simulation:
 * ./waf --run "evaluate_11ay_su_mimo"
 *
 * To increase the number of combinations that are tested in the MIMO phase, run the following command:
 * ./waf --run "evaluate_11ay_su_mimo --qdChannelFolder=SU-MIMO-Scenarios/su2x2Mimo3cm/Output/Ns3
 * --arrayConfig=28x_AzEl_SU-MIMO_2x2_27 --useAwvs=false --numStreams=2 --kBestCombinations=85 --simulationTime=5"
 *
 * Simulation Output:
 * The simulation generates the following traces:
 * 1. SNR data for all the data packets.
 * 2. SU-MIMO SISO and MIMO phases traces.
 * 3. PCAP traces for each station.
 */

NS_LOG_COMPONENT_DEFINE ("Evaluate11aySU-MIMO");

using namespace ns3;
using namespace std;

/**  Application Variables **/
string applicationType = "onoff";          /* Type of the Tx application */
uint64_t totalRx = 0;
double throughput = 0;
Ptr<PacketSink> packetSink;
Ptr<OnOffApplication> onoff;
Ptr<BulkSendApplication> bulk;

/* Network Nodes */
Ptr<WifiNetDevice> apWifiNetDevice, staWifiNetDevice;
Ptr<DmgApWifiMac> apWifiMac;
Ptr<DmgStaWifiMac> staWifiMac;
Ptr<DmgWifiPhy> apWifiPhy, staWifiPhy;
Ptr<WifiRemoteStationManager> apRemoteStationManager, staRemoteStationManager;
NetDeviceContainer staDevices;

/* Flow monitor */
Ptr<FlowMonitor> monitor;

/* Statistics */
uint64_t macTxDataFailed = 0;
uint64_t transmittedPackets = 0;
uint64_t droppedPackets = 0;
uint64_t receivedPackets = 0;
bool csv = false;                               /* Enable CSV output. */

/* SU-MIMO variables */
uint32_t kBestCombinations = 10;                /* The number of K best candidates to test in the MIMO phase . */
uint8_t numberOfTxCombinationsRequested = 10;   /* The number of Tx combinations to feedback. */
bool useAwvs = false;                           /* Flag to indicate whether we test AWVs in MIMO phase or not. */
std::string tracesFolder = "Traces/";           /* Directory to store the traces. */

/* Tracing */
Ptr<QdPropagationEngine> qdPropagationEngine;   /* Q-D Propagation Engine. */
AsciiTraceHelper ascii;
struct MIMO_PARAMETERS : public SimpleRefCount<MIMO_PARAMETERS> {
  uint32_t srcNodeID;
  uint32_t dstNodeID;
  Ptr<DmgWifiMac> srcWifiMac;
  Ptr<DmgWifiMac> dstWifiMac;
};

/*** Beamforming Service Periods ***/
uint8_t beamformedLinks = 0;                    /* Number of beamformed links */
bool firstDti = true;
bool suMimoCompleted = false;

void
CalculateThroughput (Ptr<OutputStreamWrapper> throughputOutput)
{
  double thr = CalculateSingleStreamThroughput (packetSink, totalRx, throughput);
  if (!csv)
    {
      string duration = to_string_with_precision<double> (Simulator::Now ().GetSeconds () - 0.1, 1)
                      + " - " + to_string_with_precision<double> (Simulator::Now ().GetSeconds (), 1);
      std::cout << std::left << std::setw (12) << duration
                << std::left << std::setw (12) << thr
                << std::left << std::setw (12) << qdPropagationEngine->GetCurrentTraceIndex () << std::endl;
    }
  else
    {
      std::cout << to_string_with_precision<double> (Simulator::Now ().GetSeconds (), 1) << "," << thr << std::endl;
    }
  *throughputOutput->GetStream () << to_string_with_precision<double> (Simulator::Now ().GetSeconds (), 1);
  *throughputOutput->GetStream () << "," << thr << std::endl;
  Simulator::Schedule (MilliSeconds (100), &CalculateThroughput, throughputOutput);
}

void
SLSCompleted (Ptr<OutputStreamWrapper> stream, Ptr<SLS_PARAMETERS> parameters,
              SlsCompletionAttrbitutes attributes)
{
  *stream->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                        << qdPropagationEngine->GetCurrentTraceIndex () << ","
                        << uint16_t (attributes.sectorID) << "," << uint16_t (attributes.antennaID)  << ","
                        << parameters->wifiMac->GetTypeOfStation ()  << ","
                        << apWifiNetDevice->GetNode ()->GetId () + 1  << ","
                        << Simulator::Now ().GetNanoSeconds () << std::endl;

  if (!csv)
    {
      std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
                << " completed SLS phase with EDMG STA " << attributes.peerStation << std::endl;
      std::cout << "Best Tx Antenna Configuration: AntennaID=" << uint16_t (attributes.antennaID)
                << ", SectorID=" << uint16_t (attributes.sectorID) << std::endl;
      parameters->wifiMac->PrintSnrTable ();
      if (attributes.accessPeriod == CHANNEL_ACCESS_DTI)
          {
            beamformedLinks++;
          }
//      if (beamformedLinks == 2)
//        {
//          std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
//                    << " initiating SU-MIMO BFT EDMG STA " << attributes.peerStation <<  " at " << Simulator::Now ().GetSeconds () << std::endl;
//          Ptr<Codebook> initiatorCodebook = parameters->wifiMac->GetCodebook ();
//          std::vector<AntennaID> antennas = initiatorCodebook->GetTotalAntennaIdList ();
//          Simulator::Schedule (Seconds (3), &DmgWifiMac::StartSuMimoBeamforming, parameters->wifiMac,
//                               attributes.peerStation, true, antennas);
//        }
    }
}

void
MacRxOk (Ptr<OutputStreamWrapper> stream, WifiMacType macType, Mac48Address, double snrValue)
{
  if (macType == WIFI_MAC_QOSDATA)
    {
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds () << "," << snrValue << std::endl;
    }
}

void
StationAssoicated (Ptr<DmgWifiMac> staWifiMac, Mac48Address address, uint16_t aid)
{
  if (!csv)
    {
      std::cout << "EDMG STA " << staWifiMac->GetAddress () << " associated with EDMG PCP/AP " << address
                << ", Association ID (AID) = " << aid << std::endl;
    }
}

void
MacTxDataFailed (Mac48Address)
{
  macTxDataFailed++;
}

void
PhyTxEnd (Ptr<const Packet>)
{
  transmittedPackets++;
}

void
PhyRxDrop (Ptr<const Packet> packet, WifiPhyRxfailureReason reason)
{
  droppedPackets++;
}

void
PhyRxEnd (Ptr<const Packet>)
{
  receivedPackets++;
}

void
SuMimoSisoPhaseMeasurements (Ptr<SLS_PARAMETERS> parameters, Mac48Address from, SU_MIMO_SNR_MAP measurementsMap, uint8_t edmgTrnN)
{

  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " reporting SISO phase measurements of SU-MIMO BFT with EDMG STA " << from << " at " << Simulator::Now ().GetSeconds () << std::endl;
  /* Save the SISO measuremnts to a trace file */
  Ptr<OutputStreamWrapper> outputSisoPhase = ascii.CreateFileStream (tracesFolder + "SuMimoSisoPhaseMeasurements_" + std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputSisoPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,RX_ANTENNA_ID,TX_ANTENNA_ID,TX_SECTOR_ID,SNR,Timestamp" << std::endl;
  SNR_LIST_ITERATOR start;
  for (SU_MIMO_SNR_MAP::iterator it = measurementsMap.begin (); it != measurementsMap.end (); it++)
    {
      start = it->second.begin();
      SNR_LIST_ITERATOR snrIt = it->second.begin ();
      while (snrIt != it->second.end ())
        {
          uint32_t awv = std::distance(start,snrIt) + 1;
          *outputSisoPhase->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                << qdPropagationEngine->GetCurrentTraceIndex () << ","
                                << uint16_t (std::get<1> (it->first)) << "," << uint16_t (std::get<2> (it->first))  << ","
                                << uint16_t (awv / edmgTrnN)  << "," <<  RatioToDb (*snrIt)  << ","
                                << Simulator::Now ().GetNanoSeconds () << std::endl;
          snrIt++;
        }
    }
}

void
SuMimoSisoPhaseComplete (Ptr<SLS_PARAMETERS> parameters, Mac48Address from, MIMO_FEEDBACK_MAP feedbackMap,
                         uint8_t numberOfTxAntennas, uint8_t numberOfRxAntennas)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " finished SISO phase of SU-MIMO BFT with EDMG STA " << from << " at " << Simulator::Now ().GetSeconds () << std::endl;
  /* Save the SISO feedback measuremnts to a trace file */
  Ptr<OutputStreamWrapper> outputSisoPhase = ascii.CreateFileStream (tracesFolder + "SuMimoSisoPhaseResults_"
                                                                     + std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputSisoPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,RX_ANTENNA_ID,TX_ANTENNA_ID,TX_SECTOR_ID,SNR,Timestamp" << std::endl;
  for (MIMO_FEEDBACK_MAP::iterator it = feedbackMap.begin (); it != feedbackMap.end (); it++)
    {
      *outputSisoPhase->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                            << qdPropagationEngine->GetCurrentTraceIndex () << ","
                            << uint16_t (std::get<1> (it->first)) << "," << uint16_t (std::get<0> (it->first))  << ","
                            << uint16_t (std::get<2> (it->first))  << "," <<  RatioToDb ((it->second))  << ","
                            << Simulator::Now ().GetNanoSeconds () << std::endl;
    }
  MIMO_ANTENNA_COMBINATIONS_LIST mimoCandidates =
      parameters->wifiMac->FindKBestCombinations (kBestCombinations, numberOfTxAntennas, numberOfRxAntennas, feedbackMap);
  //mimoCandidates.erase (mimoCandidates.begin (), mimoCandidates.begin () + 10);
  /* Append 5 AWVs to each sector in the codebook, increasing the granularity of steering to 5 degrees */
  if (useAwvs)
    DynamicCast<CodebookParametric> (parameters->wifiMac->GetCodebook ())->AppendAwvsForSuMimoBFT_27 ();
  parameters->wifiMac->StartSuMimoMimoPhase (from, mimoCandidates, numberOfTxCombinationsRequested, useAwvs);
}

void
SuMimoMimoCandidatesSelected (Ptr<SLS_PARAMETERS> parameters, Mac48Address from, Antenna2SectorList txCandidates, Antenna2SectorList rxCandidates)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " reporting MIMO candidates Selection for SU-MIMO BFT with EDMG STA " << from
            << " at " << Simulator::Now ().GetSeconds () << std::endl;
  /* Save the MIMO candidates to a trace file */
  Ptr<OutputStreamWrapper> outputMimoTxCandidates = ascii.CreateFileStream (tracesFolder + "SuMimoMimoTxCandidates_" +
                                                                            std::to_string (parameters->srcNodeID + 1) + ".csv");
  uint8_t numberOfAntennas = txCandidates.size ();
   *outputMimoTxCandidates->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,";
  for (uint8_t i = 1; i <= numberOfAntennas; i++)
    {
      *outputMimoTxCandidates->GetStream () << "ANTENNA_ID" << uint16_t(i) << ",SECTOR_ID" << uint16_t (i) << ",";
    }
  *outputMimoTxCandidates->GetStream () << std::endl;
  uint16_t numberOfCandidates = txCandidates.begin ()->second.size ();
  for (uint16_t i = 0; i < numberOfCandidates; i++)
    {
      *outputMimoTxCandidates->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                            << qdPropagationEngine->GetCurrentTraceIndex () << ",";
      for (Antenna2SectorListI it = txCandidates.begin (); it != txCandidates.end (); it++)
        {
          *outputMimoTxCandidates->GetStream () << uint16_t (it->first) << "," << uint16_t (it->second.at (i)) << ",";
        }
      *outputMimoTxCandidates->GetStream () << std::endl;
    }
  Ptr<OutputStreamWrapper> outputMimoRxCandidates = ascii.CreateFileStream (tracesFolder + "SuMimoMimoRxCandidates_" +
                                                                            std::to_string (parameters->srcNodeID + 1) + ".csv");
  *outputMimoRxCandidates->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,";
  for (uint8_t i = 1; i <= numberOfAntennas; i++)
    {
      *outputMimoRxCandidates->GetStream () << "ANTENNA_ID" << uint16_t(i) << ",SECTOR_ID" << uint16_t(i) << ",";
    }
  *outputMimoRxCandidates->GetStream () << std::endl;
  numberOfCandidates = rxCandidates.begin ()->second.size ();
  for (uint16_t i = 0; i < numberOfCandidates; i++)
    {
      *outputMimoRxCandidates->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                            << qdPropagationEngine->GetCurrentTraceIndex () << ",";
      for (Antenna2SectorListI it = rxCandidates.begin (); it != rxCandidates.end (); it++)
        {
          *outputMimoRxCandidates->GetStream () << uint16_t (it->first) << "," << uint16_t (it->second.at (i)) << ",";
        }
      *outputMimoRxCandidates->GetStream () << std::endl;
    }
}

void
SuMimoMimoPhaseMeasurements (Ptr<MIMO_PARAMETERS> parameters, Mac48Address from, MIMO_SNR_LIST mimoMeasurements,
                             SNR_MEASUREMENT_AWV_IDs_QUEUE minSnr, bool differentRxConfigs,
                             uint8_t nTxAntennas, uint8_t nRxAntennas, uint8_t rxCombinationsTested)
{
  std::cout << "EDMG STA " << parameters->srcWifiMac->GetAddress ()
            << " reporting MIMO phase measurements for SU-MIMO BFT with EDMG STA " << from
            << " at " << Simulator::Now ().GetSeconds () << std::endl;
  Ptr<OutputStreamWrapper> outputMimoPhase = ascii.CreateFileStream (tracesFolder + "SuMimoMimoPhaseMeasurements_" +
                                                                 std::to_string (parameters->srcNodeID + 1) + ".csv");
   *outputMimoPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,";
  for (uint8_t i = 1; i <= nTxAntennas; i++)
    {
      *outputMimoPhase->GetStream () << "TX_ANTENNA_ID" << uint16_t(i) << ",TX_SECTOR_ID" << uint16_t(i) << ",TX_AWV_ID" << uint16_t(i) << ",";
    }
  for (uint8_t i = 1; i <= nRxAntennas; i++)
    {
      *outputMimoPhase->GetStream () << "RX_ANTENNA_ID" << uint16_t(i) << ",RX_SECTOR_ID" << uint16_t(i) << ",RX_AWV_ID" << uint16_t(i) << ",";
    }
  for (uint8_t i = 0; i < nRxAntennas * nTxAntennas; i++)
    {
      *outputMimoPhase->GetStream () << "SNR,";
    }
  *outputMimoPhase->GetStream () << "min_Stream_SNR" << std::endl;
  while (!minSnr.empty ())
    {
      MEASUREMENT_AWV_IDs awvId = minSnr.top ().second;
      MIMO_AWV_CONFIGURATION rxCombination =
          parameters->srcWifiMac->GetCodebook ()->GetMimoConfigFromRxAwvId (awvId.second, from);
      MIMO_AWV_CONFIGURATION txCombination =
          parameters->dstWifiMac->GetCodebook ()->GetMimoConfigFromTxAwvId (awvId.first, parameters->srcWifiMac->GetAddress ());
      uint16_t txId = awvId.first;
      MIMO_SNR_LIST measurements;
      for (auto & rxId : awvId.second)
        {
          measurements.push_back (mimoMeasurements.at ((txId - 1) * rxCombinationsTested + rxId.second - 1));
        }
      *outputMimoPhase->GetStream () << parameters->srcNodeID + 1 << "," << parameters->dstNodeID + 1 << ","
                                     << qdPropagationEngine->GetCurrentTraceIndex () << ",";
      for (uint8_t i = 0; i < nTxAntennas; i ++)
        {
          *outputMimoPhase->GetStream () << uint16_t (txCombination.at (i).first.first)
                                         << "," << uint16_t (txCombination.at (i).first.second)
                                         << "," << uint16_t (txCombination.at (i).second) << ",";
        }
      for (uint8_t i = 0; i < nRxAntennas; i++)
        {
          *outputMimoPhase->GetStream () << uint16_t (rxCombination.at (i).first.first)
                                         << "," << uint16_t (rxCombination.at (i).first.second)
                                         << "," << uint16_t (rxCombination.at (i).second) << ",";
        }
      uint8_t snrIndex = 0;
      for (uint8_t i = 0; i < nTxAntennas; i++)
        {
          for (uint8_t j = 0; j < nRxAntennas; j++)
            {
              *outputMimoPhase->GetStream () << RatioToDb (measurements.at (j).second.at (snrIndex)) << ",";
              snrIndex++;
            }
        }
      *outputMimoPhase->GetStream () << RatioToDb (minSnr.top ().first) << std::endl;
      minSnr.pop ();
    }
}

void
SuMimoMimoPhaseComplete (Ptr<SLS_PARAMETERS> parameters, Mac48Address from)
{
  std::cout << "EDMG STA " << parameters->wifiMac->GetAddress ()
            << " finished MIMO phase of SU-MIMO BFT with EDMG STA " << from << " at " << Simulator::Now ().GetSeconds () << std::endl;
  suMimoCompleted = true;
//    if (applicationType == "onoff")
//      {
//        onoff->StartApplication ();
//      }
//    else
//      {
//        bulk->StartApplication ();
//      }
}

void
DataTransmissionIntervalStarted (Ptr<DmgStaWifiMac> wifiMac, Mac48Address address, Time dtiDuration)
{
  if (wifiMac->IsAssociated () && firstDti)
    {
      wifiMac->Perform_TXSS_TXOP (wifiMac->GetBssid ());
      firstDti = false;
    }
  if ((beamformedLinks == 2) && (Simulator::Now () > Seconds (0.6)) && !suMimoCompleted)
    {
      std::cout << "EDMG STA " << wifiMac->GetAddress ()
                << " initiating SU-MIMO BFT EDMG STA " << wifiMac->GetBssid () <<  " at " << Simulator::Now ().GetSeconds () << std::endl;
      Ptr<Codebook> initiatorCodebook = wifiMac->GetCodebook ();
      std::vector<AntennaID> antennas = initiatorCodebook->GetTotalAntennaIdList ();
      /* Start the SU-MIMO BFT protocol */
      Simulator::Schedule (MicroSeconds (3), &DmgWifiMac::StartSuMimoBeamforming, wifiMac,
                           wifiMac->GetBssid (), true, antennas, false);
    }
}

int
main (int argc, char *argv[])
{
  bool activateApp = true;                          /* Flag to indicate whether we activate onoff or bulk App */
  string socketType = "ns3::UdpSocketFactory";      /* Socket Type (TCP/UDP) */
  uint32_t packetSize = 1448;                       /* Application payload size in bytes. */
  string tcpVariant = "NewReno";                    /* TCP Variant Type. */
  uint32_t bufferSize = 131072;                     /* TCP Send/Receive Buffer Size. */
  uint32_t maxPackets = 0;                          /* Maximum Number of Packets */
  string msduAggSize = "max";                       /* The maximum aggregation size for A-MSDU in Bytes. */
  string mpduAggSize = "max";                       /* The maximum aggregation size for A-MPDU in Bytes. */
  string queueSize = "4000p";                       /* Wifi MAC Queue Size. */
  uint32_t numStreams = 2;                          /* The total number of spatial streams in the network. */
  uint32_t channelNumber = 2;                       /* The channel number of the network. */
  double txPower = 10;                              /* The transmit power in dBm of the devices. */
  string phyMode = "EDMG_SC_MCS1";                  /* Type of the Physical Layer. */
  bool verbose = false;                             /* Print Logging Information. */
  double simulationTime = 10;                       /* Simulation time in seconds. */
  bool pcapTracing = false;                         /* PCAP Tracing is enabled or not. */
  std::string arrayConfig = "28x_AzEl_SU-MIMO_2x2_27";  /* Phased antenna array configuration*/
  std::string qdChannelFolder = "SU-MIMO-Scenarios/su2x2Mimo3cm/Output/Ns3";/* Path to the folder containing SU-MIMO Q-D files. */
  uint32_t traceIndex = 0;                          /* Trace Index in the Q-D file. */

  /* Command line argument parser setup. */
  CommandLine cmd;
  cmd.AddValue ("activateApp", "Whether to activate data transmission or not", activateApp);
  cmd.AddValue ("applicationType", "Type of the Tx Application: onoff or bulk", applicationType);
  cmd.AddValue ("packetSize", "Application packet size in bytes", packetSize);
  cmd.AddValue ("maxPackets", "Maximum number of packets to send", maxPackets);
  cmd.AddValue ("tcpVariant", TCP_VARIANTS_NAMES, tcpVariant);
  cmd.AddValue ("socketType", "Type of the Socket (ns3::TcpSocketFactory, ns3::UdpSocketFactory)", socketType);
  cmd.AddValue ("bufferSize", "TCP Buffer Size (Send/Receive) in Bytes", bufferSize);
  cmd.AddValue ("msduAggSize", "The maximum aggregation size for A-MSDU in Bytes", msduAggSize);
  cmd.AddValue ("msduAggSize", "The maximum aggregation size for A-MPDU in Bytes", msduAggSize);
  cmd.AddValue ("numStreams", "The number of spatial streams in the network. It will be used by the onoff application"
                              " to determine its datarate", numStreams);
  cmd.AddValue ("queueSize", "The maximum size of the Wifi MAC Queue", queueSize);
  cmd.AddValue ("kBestCombinations", "The number of K best candidates to test in the MIMO phase", kBestCombinations);
  cmd.AddValue ("nTxCombinations", "The number of Tx combinations to feedback", numberOfTxCombinationsRequested);
  cmd.AddValue ("useAwvs", "Flag to indicate whether we test AWVs in MIMO phase or not", useAwvs);
  cmd.AddValue ("channelNumber", "The channel number of the network", channelNumber);
  cmd.AddValue ("txPower", "The transmit power in dBm of the devices", txPower);
  cmd.AddValue ("phyMode", "802.11ay PHY Mode", phyMode);
  cmd.AddValue ("verbose", "Turn on all WifiNetDevice log components", verbose);
  cmd.AddValue ("qdChannelFolder", "Path to the Q-D files describing the SU-MIMO scenario", qdChannelFolder);
  cmd.AddValue ("tracesFolder", "Path to the folder where we dump all the traces", tracesFolder);
  cmd.AddValue ("simulationTime", "Simulation time in seconds", simulationTime);
  cmd.AddValue ("pcap", "Enable PCAP Tracing", pcapTracing);
  cmd.AddValue ("arrayConfig", "Antenna array configuration", arrayConfig);
  cmd.AddValue ("traceIndex", "The Trace Index in the Q-D file", traceIndex);
  cmd.AddValue ("csv", "Enable CSV output instead of plain text. This mode will suppress all the messages related statistics and events.", csv);
  cmd.Parse (argc, argv);

  /* Validate A-MSDU and A-MPDU values */
  ValidateFrameAggregationAttributes (msduAggSize, mpduAggSize, WIFI_PHY_STANDARD_80211ay);
  /* Configure RTS/CTS and Fragmentation */
  ConfigureRtsCtsAndFragmenatation ();
  /* Wifi MAC Queue Parameters */
  ChangeQueueSize (queueSize);

  /*** Configure TCP Options ***/
  ConfigureTcpOptions (tcpVariant, packetSize, bufferSize);

  /**** DmgWifiHelper is a meta-helper ****/
  DmgWifiHelper wifi;

  /* Basic setup */
  wifi.SetStandard (WIFI_PHY_STANDARD_80211ay);

  /* Turn on logging */
  if (verbose)
    {
      wifi.EnableLogComponents ();
    }

  /**** Setup mmWave Q-D Channel ****/
  /**** Set up Channel ****/
  Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel> ();
  qdPropagationEngine = CreateObject<QdPropagationEngine> ();
  qdPropagationEngine->SetAttribute ("QDModelFolder", StringValue ("DmgFiles/QdChannel/" + qdChannelFolder + "/"));
  Ptr<QdPropagationLossModel> lossModelRaytracing = CreateObject<QdPropagationLossModel> (qdPropagationEngine);
  Ptr<QdPropagationDelayModel> propagationDelayRayTracing = CreateObject<QdPropagationDelayModel> (qdPropagationEngine);
  spectrumChannel->AddSpectrumPropagationLossModel (lossModelRaytracing);
  spectrumChannel->SetPropagationDelayModel (propagationDelayRayTracing);
  qdPropagationEngine->SetAttribute ("StartIndex", UintegerValue (traceIndex));

  /**** Setup physical layer ****/
  SpectrumDmgWifiPhyHelper spectrumWifiPhy = SpectrumDmgWifiPhyHelper::Default ();
  spectrumWifiPhy.SetChannel (spectrumChannel);
  /* All nodes transmit at 10 dBm == 10 mW, no adaptation */
  spectrumWifiPhy.Set ("TxPowerStart", DoubleValue (txPower));
  spectrumWifiPhy.Set ("TxPowerEnd", DoubleValue (txPower));
  spectrumWifiPhy.Set ("TxPowerLevels", UintegerValue (1));
  /* Set operating channel */
  EDMG_CHANNEL_CONFIG config = FindChannelConfiguration (channelNumber);
  spectrumWifiPhy.Set ("ChannelNumber", UintegerValue (config.chNumber));
  spectrumWifiPhy.Set ("PrimaryChannelNumber", UintegerValue (config.primayChannel));
  /* Set the correct error model */
  spectrumWifiPhy.SetErrorRateModel ("ns3::DmgErrorModel",
                                     "FileName", StringValue ("DmgFiles/ErrorModel/LookupTable_1458_ay.txt"));
  /* Enable support for SU-MIMO */
  spectrumWifiPhy.Set ("SupportSuMimo", BooleanValue (true));
  /* Set default algorithm for all nodes to be constant rate */
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager", "DataMode", StringValue (phyMode));
  /* Make four nodes and set them up with the phy and the mac */
  NodeContainer wifiNodes;
  wifiNodes.Create (2);
  Ptr<Node> apWifiNode = wifiNodes.Get (0);
  Ptr<Node> staWifiNode = wifiNodes.Get (1);

  /* Add a DMG upper mac */
  DmgWifiMacHelper wifiMac = DmgWifiMacHelper::Default ();

  /* Install DMG PCP/AP Node */
  Ssid ssid = Ssid ("SU-MIMO");
  wifiMac.SetType ("ns3::DmgApWifiMac",
                   "Ssid", SsidValue (ssid),
                   "BE_MaxAmpduSize", StringValue (mpduAggSize),
                   "BE_MaxAmsduSize", StringValue (msduAggSize),
                   "SSSlotsPerABFT", UintegerValue (8), "SSFramesPerSlot", UintegerValue (16),
                   "BeaconInterval", TimeValue (MicroSeconds (102400)),
                   "EDMGSupported", BooleanValue (true));

  /* Set Parametric Codebook for the EDMG AP */
  wifi.SetCodebook ("ns3::CodebookParametric",
                    "MimoCodebook", BooleanValue (true),
                    "TotalAntennas", UintegerValue (numStreams),
                    "FileName", StringValue ("DmgFiles/Codebook/CODEBOOK_URA_AP_" + arrayConfig + ".txt"));

  /* Create Wifi Network Devices (WifiNetDevice) */
  NetDeviceContainer apDevice;
  apDevice = wifi.Install (spectrumWifiPhy, wifiMac, apWifiNode);

  wifiMac.SetType ("ns3::DmgStaWifiMac",
                   "Ssid", SsidValue (ssid), "ActiveProbing", BooleanValue (false),
                   "BE_MaxAmpduSize", StringValue (mpduAggSize),
                   "BE_MaxAmsduSize", StringValue (msduAggSize),
                   "EDMGSupported", BooleanValue (true));

  /* Set Parametric Codebook for the EDMG STA */
  wifi.SetCodebook ("ns3::CodebookParametric",
                    "MimoCodebook", BooleanValue (true),
                    "TotalAntennas", UintegerValue (numStreams),
                    "FileName", StringValue ("DmgFiles/Codebook/CODEBOOK_URA_STA_" + arrayConfig + ".txt"));

  staDevices = wifi.Install (spectrumWifiPhy, wifiMac, staWifiNode);

  /* Setting mobility model */
  MobilityHelper mobility;
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (wifiNodes);

  /* Internet stack*/
  InternetStackHelper stack;
  stack.Install (wifiNodes);

  Ipv4AddressHelper address;
  address.SetBase ("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apInterface;
  apInterface = address.Assign (apDevice);
  Ipv4InterfaceContainer staInterfaces;
  staInterfaces = address.Assign (staDevices);

  /* We do not want any ARP packets */
  PopulateArpCache ();

  if (activateApp)
    {
      /* Install Simple UDP Server on the DMG AP */
      PacketSinkHelper sinkHelper (socketType, InetSocketAddress (Ipv4Address::GetAny (), 9999));
      ApplicationContainer sinkApp = sinkHelper.Install (apWifiNode);
      packetSink = StaticCast<PacketSink> (sinkApp.Get (0));
      sinkApp.Start (Seconds (0.0));

      /* Install TCP/UDP Transmitter on the DMG STA */
      Address dest (InetSocketAddress (apInterface.GetAddress (0), 9999));
      ApplicationContainer srcApp;
      if (applicationType == "onoff")
        {
          WifiMode mode = WifiMode (phyMode);
          OnOffHelper src (socketType, dest);
          src.SetAttribute ("MaxPackets", UintegerValue (maxPackets));
          src.SetAttribute ("PacketSize", UintegerValue (packetSize));
          src.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1e6]"));
          src.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
          src.SetAttribute ("DataRate", DataRateValue (DataRate (mode.GetPhyRate () * config.NCB * numStreams)));
          srcApp = src.Install (staWifiNode);
          onoff = StaticCast<OnOffApplication> (srcApp.Get (0));
        }
      else if (applicationType == "bulk")
        {
          BulkSendHelper src (socketType, dest);
          srcApp= src.Install (staWifiNode);
          bulk = StaticCast<BulkSendApplication> (srcApp.Get (0));
        }
      srcApp.Start (Seconds(0.01));
      srcApp.Stop (Seconds (simulationTime));
    }

  /* Enable Traces */
  if (pcapTracing)
    {
      spectrumWifiPhy.SetPcapDataLinkType (YansWifiPhyHelper::DLT_IEEE802_11_RADIO);
      spectrumWifiPhy.SetSnapshotLength (120);
      spectrumWifiPhy.EnablePcap ("Traces/AccessPoint", apDevice, false);
      spectrumWifiPhy.EnablePcap ("Traces/StaNode", staDevices.Get (0), false);
    }

  /* Stations */
  apWifiNetDevice = StaticCast<WifiNetDevice> (apDevice.Get (0));
  staWifiNetDevice = StaticCast<WifiNetDevice> (staDevices.Get (0));
  apRemoteStationManager = StaticCast<WifiRemoteStationManager> (apWifiNetDevice->GetRemoteStationManager ());
  apWifiMac = StaticCast<DmgApWifiMac> (apWifiNetDevice->GetMac ());
  staWifiMac = StaticCast<DmgStaWifiMac> (staWifiNetDevice->GetMac ());
  apWifiPhy = StaticCast<DmgWifiPhy> (apWifiNetDevice->GetPhy ());
  staWifiPhy = StaticCast<DmgWifiPhy> (staWifiNetDevice->GetPhy ());
  staRemoteStationManager = StaticCast<WifiRemoteStationManager> (staWifiNetDevice->GetRemoteStationManager ());

  /** Connect Traces **/
  AsciiTraceHelper ascii;

  /* EDMG AP Straces */
  Ptr<OutputStreamWrapper> outputSlsPhase = CreateSlsTraceStream (tracesFolder + "slsResults" + arrayConfig);
  *outputSlsPhase->GetStream () << "SRC_ID,DST_ID,TRACE_IDX,ANTENNA_ID_1,SECTOR_ID_1,AWV_ID_1,"
                                   "ANTENNA_ID_2,SECTOR_ID_2,AWV_ID_2,ROLE,BSS_ID,SNR,Timestamp" << std::endl;

  /* SLS Traces */
  Ptr<SLS_PARAMETERS> parametersAp = Create<SLS_PARAMETERS> ();
  parametersAp->srcNodeID = apWifiNetDevice->GetNode ()->GetId ();
  parametersAp->dstNodeID = staWifiNetDevice->GetNode ()->GetId ();
  parametersAp->wifiMac = apWifiMac;
  Ptr<MIMO_PARAMETERS> mimoParametersAp = Create<MIMO_PARAMETERS> ();
  mimoParametersAp->srcNodeID = apWifiNetDevice->GetNode ()->GetId ();
  mimoParametersAp->dstNodeID = staWifiNetDevice->GetNode ()->GetId ();
  mimoParametersAp->srcWifiMac = apWifiMac;
  mimoParametersAp->dstWifiMac = staWifiMac;
  apWifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("SuMimoSisoPhaseMeasurements", MakeBoundCallback (&SuMimoSisoPhaseMeasurements, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("SuMimoSisoPhaseCompleted", MakeBoundCallback (&SuMimoSisoPhaseComplete, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("SuMimoMimoCandidatesSelected", MakeBoundCallback (&SuMimoMimoCandidatesSelected, parametersAp));
  apWifiMac->TraceConnectWithoutContext ("SuMimoMimoPhaseMeasurements", MakeBoundCallback (&SuMimoMimoPhaseMeasurements, mimoParametersAp));
  apWifiMac->TraceConnectWithoutContext ("SuMimoMimoPhaseCompleted", MakeBoundCallback (&SuMimoMimoPhaseComplete, parametersAp));
  apWifiPhy->TraceConnectWithoutContext ("PhyRxEnd", MakeCallback (&PhyRxEnd));
  apWifiPhy->TraceConnectWithoutContext ("PhyRxDrop", MakeCallback (&PhyRxDrop));

  /* EDMG STA Straces */
  Ptr<SLS_PARAMETERS> parametersSta = Create<SLS_PARAMETERS> ();
  parametersSta->srcNodeID = staWifiNetDevice->GetNode ()->GetId ();
  parametersSta->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  parametersSta->wifiMac = staWifiMac;
  Ptr<MIMO_PARAMETERS> mimoParametersSta = Create<MIMO_PARAMETERS> ();
  mimoParametersSta->srcNodeID = staWifiNetDevice->GetNode ()->GetId ();
  mimoParametersSta->dstNodeID = apWifiNetDevice->GetNode ()->GetId ();
  mimoParametersSta->srcWifiMac = staWifiMac;
  mimoParametersSta->dstWifiMac = apWifiMac;
  staWifiMac->TraceConnectWithoutContext ("Assoc", MakeBoundCallback (&StationAssoicated, staWifiMac));
  staWifiMac->TraceConnectWithoutContext ("SLSCompleted", MakeBoundCallback (&SLSCompleted, outputSlsPhase, parametersSta));
  staWifiMac->TraceConnectWithoutContext ("DTIStarted", MakeBoundCallback (&DataTransmissionIntervalStarted, staWifiMac));
  staWifiMac->TraceConnectWithoutContext ("SuMimoSisoPhaseMeasurements", MakeBoundCallback (&SuMimoSisoPhaseMeasurements, parametersSta));
  staWifiMac->TraceConnectWithoutContext ("SuMimoSisoPhaseCompleted", MakeBoundCallback (&SuMimoSisoPhaseComplete, parametersSta));
  staWifiMac->TraceConnectWithoutContext ("SuMimoMimoCandidatesSelected", MakeBoundCallback (&SuMimoMimoCandidatesSelected, parametersSta));
  staWifiMac->TraceConnectWithoutContext ("SuMimoMimoPhaseMeasurements", MakeBoundCallback (&SuMimoMimoPhaseMeasurements, mimoParametersSta));
  staWifiMac->TraceConnectWithoutContext ("SuMimoMimoPhaseCompleted", MakeBoundCallback (&SuMimoMimoPhaseComplete, parametersSta));
  staWifiPhy->TraceConnectWithoutContext ("PhyTxEnd", MakeCallback (&PhyTxEnd));
  staRemoteStationManager->TraceConnectWithoutContext ("MacTxDataFailed", MakeCallback (&MacTxDataFailed));

  /* Get SNR Traces */
  Ptr<OutputStreamWrapper> snrStream = ascii.CreateFileStream (tracesFolder + "snrValues.csv");
  apRemoteStationManager->TraceConnectWithoutContext ("MacRxOK", MakeBoundCallback (&MacRxOk, snrStream));

  FlowMonitorHelper flowmon;
  if (activateApp)
    {
      /* Install FlowMonitor on all nodes */
      monitor = flowmon.InstallAll ();

      /* Print Output */
      if (!csv)
        {
          std::cout << std::left << std::setw (12) << "Time [s]"
                    << std::left << std::setw (12) << "Throughput [Mbps]" << std::endl;
        }

      /* Schedule Throughput Calulcations */
      Ptr<OutputStreamWrapper> throughputOutput;
      throughputOutput = ascii.CreateFileStream ("throughput_SU_MIMO.csv");
      Simulator::Schedule (Seconds (0.1), &CalculateThroughput, throughputOutput);
    }

  Simulator::Stop (Seconds (simulationTime + 0.101));
  Simulator::Run ();
  Simulator::Destroy ();

  if (!csv)
    {
      if (activateApp)
        {
          PrintFlowMonitorStatistics (flowmon, monitor, simulationTime - 0.1);
          /* Print Application Layer Results Summary */
          std::cout << "\nApplication Layer Statistics:" << std::endl;;
          if (applicationType == "onoff")
            {
              std::cout << "  Tx Packets: " << onoff->GetTotalTxPackets () << std::endl;
              std::cout << "  Tx Bytes:   " << onoff->GetTotalTxBytes () << std::endl;
            }
          else
            {
              std::cout << "  Tx Packets: " << bulk->GetTotalTxPackets () << std::endl;
              std::cout << "  Tx Bytes:   " << bulk->GetTotalTxBytes () << std::endl;
            }

          std::cout << "  Rx Packets: " << packetSink->GetTotalReceivedPackets () << std::endl;
          std::cout << "  Rx Bytes:   " << packetSink->GetTotalRx () << std::endl;
          std::cout << "  Throughput: " << packetSink->GetTotalRx () * 8.0 / ((simulationTime - 1) * 1e6) << " Mbps" << std::endl;
        }

      /* Print MAC Layer Statistics */
      std::cout << "\nMAC Layer Statistics:" << std::endl;;
      std::cout << "  Number of Failed Tx Data Packets:  " << macTxDataFailed << std::endl;

      /* Print PHY Layer Statistics */
      std::cout << "\nPHY Layer Statistics:" << std::endl;;
      std::cout << "  Number of Tx Packets:         " << transmittedPackets << std::endl;
      std::cout << "  Number of Rx Packets:         " << receivedPackets << std::endl;
      std::cout << "  Number of Rx Dropped Packets: " << droppedPackets << std::endl;
    }

  return 0;
}