#include "rapidjson/document.h"
#include "rapidjson/reader.h"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>

const int CONSUMER_TIMEOUT = 50;

using namespace rapidjson;

std::vector<std::thread> producer_threads;
std::vector<std::thread> consumer_threads;

std::vector<std::string> message_queue;
std::mutex rwlock;

std::string get_quality(double value, double avg, double range) {
  double diff = std::abs(value - avg);
  // extremity is ratio normalized to fit in [0, 1]
  double extremity = 2 * diff / range;
  // std::cout << "LOG: Value rolled was " << value << ", avg is " << avg
  //           << ", range is " << range << '\n';
  // std::cout << "LOG: Diff is " << diff << ", extremity is " << extremity
  //           << '\n';
  if (extremity < 0.5) {
    return "Normal";
  } else if (extremity < 0.8) {
    return "Warning";
  } else {
    return "Alarm";
  }
}

std::tuple<int, std::string, double, std::string>
read_message(std::string message) {
  std::string encoder_type, sensor_id, type, value, quality;
  std::string delimiter{", "};

  size_t pos{0};

  // extract encoder type
  pos = message.find(delimiter);
  encoder_type = message.substr(0, pos);
  message.erase(0, pos + delimiter.length());

  // extract sensor id
  pos = message.find(delimiter);
  sensor_id = message.substr(0, pos);
  message.erase(0, pos + delimiter.length());

  // extract type
  pos = message.find(delimiter);
  type = message.substr(0, pos);
  message.erase(0, pos + delimiter.length());

  // extract value
  pos = message.find(delimiter);
  value = message.substr(0, pos);
  message.erase(0, pos + delimiter.length());

  // extract quality
  pos = message.find(delimiter);
  quality = message.substr(0, pos);

  return {std::stoi(sensor_id), type, std::stod(value), quality};
}

void create_producer(Value &sensor) {
  int id = sensor["ID"].GetInt();
  std::string type = sensor["Type"].GetString();
  double min = sensor["MinValue"].GetDouble();
  double max = sensor["MaxValue"].GetDouble();
  double range = max - min;
  std::string encoder = sensor["EncoderType"].GetString();
  int freq = sensor["Frequency"].GetInt();
  double avg = (max + min) / 2;
  std::uniform_real_distribution<double> distribution(min, max);
  std::default_random_engine generator;
  while (true) {
    double value = distribution(generator);
    std::string quality = get_quality(value, avg, range);

    std::stringstream ss;
    ss << "$FIX, " << id << ", " << type << ", " << value << ", " << quality
       << "*";
    {
      std::lock_guard<std::mutex> guard(rwlock);
      message_queue.push_back(ss.str());
      // std::cout << "LOG: $FIX, " << id << ", " << type << ", " << value << ",
      // "
      //           << quality << "*" << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000 / freq));
  }
}

void create_consumer(Value &receiver) {
  int consumer_pointer{0};
  int receiver_id = receiver["ReceiverID"].GetInt();
  int listened_sensor_id = receiver["SensorID"].GetInt();
  bool is_active = receiver["Active"].GetBool();
  if (!is_active)
    return;
  while (true) {
    {
      // std::cout << "tu se jestem" << std::endl;
      {
        std::lock_guard<std::mutex> guard(rwlock);
        if (message_queue.size() > consumer_pointer) {
          auto [sensor_id, type, value, quality] =
              read_message(message_queue.at(consumer_pointer));
          if (sensor_id == listened_sensor_id) {
            if (quality == "Normal*")
              std::cout << "\033[32m";
            if (quality == "Warning*")
              std::cout << "\033[33m";
            if (quality == "Alarm*")
              std::cout << "\033[31m";

            std::cout << "Read " << type << ": " << value
                      << " from sensor ID: " << sensor_id
                      << " via receiver ID: " << receiver_id << ", quality is "
                      << quality << "\033[0m" << std::endl;
            ;
          }
          consumer_pointer++;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(CONSUMER_TIMEOUT));
    }
  }
}

int main(int argc, char **argv) {

  if (argc != 3) {
    std::cerr << "Usage: " << argv[0]
              << " producer_config.json consumer_config.json" << std::endl;
    return 1;
  }
  std::ifstream producer_config(argv[1]);
  if (!producer_config) {
    std::cerr << "Error: unable to open producer config file " << argv[1]
              << std::endl;
    return 2;
  }

  std::string producer_json((std::istreambuf_iterator<char>(producer_config)),
                            std::istreambuf_iterator<char>());
  Document producer_doc;
  producer_doc.Parse(producer_json.c_str());
  if (producer_doc.HasParseError()) {
    std::cerr << "Error: failed to parse producer config JSON" << std::endl;
    return 3;
  }

  std::ifstream consumer_config(argv[2]);
  if (!consumer_config) {
    std::cerr << "Error: unable to open consumer config file " << argv[2]
              << std::endl;
    return 2;
  }

  std::string consumer_json((std::istreambuf_iterator<char>(consumer_config)),
                            std::istreambuf_iterator<char>());
  Document consumer_doc;
  consumer_doc.Parse(consumer_json.c_str());
  if (consumer_doc.HasParseError()) {
    std::cerr << "Error: failed to parse producer config JSON" << std::endl;
    return 3;
  }

  Value &sensors = producer_doc["Sensors"];
  for (int i = 0; i < sensors.Size(); i++) {
    producer_threads.emplace_back(create_producer, std::ref(sensors[i]));
  }

  Value &receivers = consumer_doc["Receivers"];
  for (int i = 0; i < receivers.Size(); i++) {
    consumer_threads.emplace_back(create_consumer, std::ref(receivers[i]));
  }

  for (auto &thr : producer_threads) {
    thr.join();
  }

  for (auto &thr : consumer_threads) {
    thr.join();
  }

  return 0;
}
