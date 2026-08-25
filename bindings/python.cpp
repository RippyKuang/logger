#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "rtplot/downsample.hpp"
#include "rtplot/logger.hpp"
#include "rtplot/storage.hpp"

namespace py = pybind11;
using namespace rtplot;

PYBIND11_MODULE(rtplot, m) {
  m.doc() = "rtplot: lightweight lock-free realtime plotting / logging library";

  py::class_<Sample>(m, "Sample")
      .def(py::init<Timestamp, double>(), py::arg("t") = 0, py::arg("v") = 0.0)
      .def_readwrite("t", &Sample::t)
      .def_readwrite("v", &Sample::v);

  py::class_<ArraySample>(m, "ArraySample")
      .def(py::init<>())
      .def_readwrite("t", &ArraySample::t)
      .def_property("size", [](const ArraySample& s){ return s.size; }, nullptr)
      .def("__len__", [](const ArraySample& s){ return s.size; })
      .def("__getitem__", [](const ArraySample& s, size_t i){ if(i>=s.size) throw py::index_error(); return s.values[i]; });

  py::class_<Event>(m, "Event")
      .def(py::init<Timestamp, std::string, std::string>(),
           py::arg("t") = 0, py::arg("name") = "", py::arg("payload") = "")
      .def_readwrite("t", &Event::t)
      .def_readwrite("name", &Event::name)
      .def_readwrite("payload", &Event::payload);

  py::class_<Stats>(m, "Stats")
      .def_readonly("count", &Stats::count)
      .def_readonly("min", &Stats::min)
      .def_readonly("max", &Stats::max)
      .def_readonly("mean", &Stats::mean)
      .def_readonly("variance", &Stats::variance)
      .def_readonly("stddev", &Stats::stddev)
      .def_readonly("rms", &Stats::rms);

  py::enum_<DownsampleAlgorithm>(m, "DownsampleAlgorithm")
      .value("MinMax", DownsampleAlgorithm::MinMax)
      .value("LTTB", DownsampleAlgorithm::LTTB)
      .value("Decimate", DownsampleAlgorithm::Decimate);

  py::enum_<OverflowPolicy>(m, "OverflowPolicy")
      .value("DropNewest", OverflowPolicy::DropNewest)
      .value("DropOldest", OverflowPolicy::DropOldest)
      .value("Spin", OverflowPolicy::Spin);

  py::class_<LoggerConfig>(m, "LoggerConfig")
      .def(py::init<>())
      .def_readwrite("ring_capacity", &LoggerConfig::ringCapacity)
      .def_readwrite("persist", &LoggerConfig::persist)
      .def_readwrite("db_path", &LoggerConfig::dbPath)
      .def_readwrite("shm_publish", &LoggerConfig::shmPublish)
      .def_readwrite("shm_ring_capacity", &LoggerConfig::shmRingCapacity)
      .def_readwrite("shm_name", &LoggerConfig::shmName)
      .def_readwrite("udp_publish", &LoggerConfig::udpPublish)
      .def_readwrite("udp_address", &LoggerConfig::udpAddress)
      .def_readwrite("udp_port", &LoggerConfig::udpPort)
      .def_readwrite("flush_interval_ms", &LoggerConfig::flushIntervalMs)
      .def_readwrite("start_background_thread", &LoggerConfig::startBackgroundThread)
      .def_readwrite("drop_oldest_when_full", &LoggerConfig::dropOldestWhenFull)
      .def_readwrite("overflow_policy", &LoggerConfig::overflowPolicy);

  py::class_<Logger, std::unique_ptr<Logger, py::nodelete>>(m, "Logger")
      .def_static("instance", &Logger::instance, py::return_value_policy::reference)
      .def("start", [](Logger& self, const LoggerConfig& cfg) { return self.start(cfg); },
           py::arg("config") = LoggerConfig{})
      .def("stop", &Logger::stop)
      .def("running", &Logger::running)
      .def("log",
           [](Logger& self, const std::string& channel, double value, Timestamp t) {
             return self.log(channel, value, t == 0 ? nowNs() : t);
           },
           py::arg("channel"), py::arg("value"), py::arg("t") = 0)
      .def("event",
           [](Logger& self, const std::string& name, const std::string& payload, Timestamp t) {
             return self.event(name, payload, t == 0 ? nowNs() : t);
           },
           py::arg("name"), py::arg("payload") = "", py::arg("t") = 0)
      .def("log_array",
           [](Logger& self, const std::string& channel, const std::vector<double>& values, Timestamp t) {
             return self.logArray(channel, values.data(), values.size(), t == 0 ? nowNs() : t);
           },
           py::arg("channel"), py::arg("values"), py::arg("t") = 0)
      .def("flush_and_stop", &Logger::flushAndStop)
      .def("snapshot", [](Logger& self, const std::string& ch, size_t max) {
             std::vector<Sample> out;
             self.snapshot(ch, out, max);
             return out;
           }, py::arg("channel"), py::arg("max_samples") = 1 << 20)
      .def("snapshot_array", [](Logger& self, const std::string& ch, size_t max) {
             std::vector<ArraySample> out;
             self.snapshotArray(ch, out, max);
             return out;
           }, py::arg("channel"), py::arg("max_samples") = 1 << 20)
      .def_property_readonly("accepted_samples", &Logger::acceptedSamples)
      .def_property_readonly("dropped_samples", &Logger::droppedSamples);

  py::class_<StorageConfig>(m, "StorageConfig")
      .def(py::init<>())
      .def_readwrite("flush_interval_ms", &StorageConfig::flushIntervalMs)
      .def_readwrite("max_wal_bytes", &StorageConfig::maxWalBytes);

  py::class_<StorageWriter>(m, "StorageWriter")
      .def(py::init<>())
      .def("open", &StorageWriter::open, py::arg("path"), py::arg("config") = StorageConfig{})
      .def("close", &StorageWriter::close)
      .def("write_samples",
           [](StorageWriter& w, const std::string& channel, const std::vector<Sample>& s) {
             return w.writeSamples(channel, s.data(), s.size());
           })
      .def("write_event", &StorageWriter::writeEvent)
      .def("write_array_samples",
           [](StorageWriter& w, const std::string& channel, const std::vector<std::vector<double>>& samples) {
             std::vector<ArraySample> v;
             for (size_t i = 0; i < samples.size(); ++i) {
               ArraySample a; a.t = static_cast<Timestamp>(i); a.size = static_cast<uint32_t>(samples[i].size());
               for (size_t j = 0; j < samples[i].size() && j < kMaxArrayLength; ++j) a.values[j] = samples[i][j];
               v.push_back(a);
             }
             return w.writeArraySamples(channel, v.data(), v.size());
           })
      .def("flush", &StorageWriter::flush);

  py::class_<StorageReader>(m, "StorageReader")
      .def(py::init<>())
      .def("open", &StorageReader::open, py::arg("path"))
      .def("close", &StorageReader::close)
      .def("channels", [](StorageReader& r) {
             std::vector<std::string> out;
             for (const auto& c : r.channels()) out.push_back(c.name);
             return out;
           })
      .def("read_samples",
           [](StorageReader& r, const std::string& channel, Timestamp t0, Timestamp t1, size_t max) {
             return r.readSamples(channel, t0, t1, max);
           },
           py::arg("channel"), py::arg("t0") = 0, py::arg("t1") = INT64_MAX,
           py::arg("max_samples") = 0)
      .def("events", &StorageReader::events, py::arg("t0") = 0, py::arg("t1") = INT64_MAX)
      .def("read_array_samples", &StorageReader::readArraySamples,
           py::arg("channel"), py::arg("t0") = 0, py::arg("t1") = INT64_MAX,
           py::arg("max_samples") = 0)
      .def("stats", &StorageReader::stats, py::arg("channel"), py::arg("t0") = 0,
           py::arg("t1") = INT64_MAX);

  m.def("now_ns", &nowNs);
  m.def("compute_stats", [](const std::vector<Sample>& s) { return computeStats(s); });
  m.def("downsample",
        [](const std::vector<Sample>& s, size_t maxPoints, DownsampleAlgorithm algo) {
          return downsample(s, maxPoints, algo);
        },
        py::arg("samples"), py::arg("max_points"),
        py::arg("algorithm") = DownsampleAlgorithm::MinMax);
  m.def("export_csv", &exportCsv, py::arg("db_path"), py::arg("csv_path"), py::arg("delimiter") = ',');

}
