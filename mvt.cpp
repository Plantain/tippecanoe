#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <zlib.h>
#include "mvt.hpp"
#include "protozero/varint.hpp"
#include "protozero/pbf_reader.hpp"
#include "protozero/pbf_writer.hpp"

mvt_geometry::mvt_geometry(int nop, long long nx, long long ny) {
	this->op = nop;
	this->x = nx;
	this->y = ny;
}

// https://github.com/mapbox/mapnik-vector-tile/blob/master/src/vector_tile_compression.hpp
bool is_compressed(std::string const &data) {
	return data.size() > 2 && (((uint8_t) data[0] == 0x78 && (uint8_t) data[1] == 0x9C) || ((uint8_t) data[0] == 0x1F && (uint8_t) data[1] == 0x8B));
}

// https://github.com/mapbox/mapnik-vector-tile/blob/master/src/vector_tile_compression.hpp
int decompress(std::string const &input, std::string &output) {
	z_stream inflate_s;
	inflate_s.zalloc = Z_NULL;
	inflate_s.zfree = Z_NULL;
	inflate_s.opaque = Z_NULL;
	inflate_s.avail_in = 0;
	inflate_s.next_in = Z_NULL;
	if (inflateInit2(&inflate_s, 32 + 15) != Z_OK) {
		fprintf(stderr, "error: %s\n", inflate_s.msg);
	}
	inflate_s.next_in = (Bytef *) input.data();
	inflate_s.avail_in = input.size();
	size_t length = 0;
	do {
		output.resize(length + 2 * input.size());
		inflate_s.avail_out = 2 * input.size();
		inflate_s.next_out = (Bytef *) (output.data() + length);
		int ret = inflate(&inflate_s, Z_FINISH);
		if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
			fprintf(stderr, "error: %s\n", inflate_s.msg);
			return 0;
		}

		length += (2 * input.size() - inflate_s.avail_out);
	} while (inflate_s.avail_out == 0);
	inflateEnd(&inflate_s);
	output.resize(length);
	return 1;
}

// https://github.com/mapbox/mapnik-vector-tile/blob/master/src/vector_tile_compression.hpp
int compress(std::string const &input, std::string &output) {
	z_stream deflate_s;
	deflate_s.zalloc = Z_NULL;
	deflate_s.zfree = Z_NULL;
	deflate_s.opaque = Z_NULL;
	deflate_s.avail_in = 0;
	deflate_s.next_in = Z_NULL;
	deflateInit2(&deflate_s, Z_BEST_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
	deflate_s.next_in = (Bytef *) input.data();
	deflate_s.avail_in = input.size();
	size_t length = 0;
	do {
		size_t increase = input.size() / 2 + 1024;
		output.resize(length + increase);
		deflate_s.avail_out = increase;
		deflate_s.next_out = (Bytef *) (output.data() + length);
		int ret = deflate(&deflate_s, Z_FINISH);
		if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
			return -1;
		}
		length += (increase - deflate_s.avail_out);
	} while (deflate_s.avail_out == 0);
	deflateEnd(&deflate_s);
	output.resize(length);
	return 0;
}

bool mvt_tile::decode(std::string &message) {
	layers.clear();
	std::string src;

	if (is_compressed(message)) {
		std::string uncompressed;
		decompress(message, uncompressed);
		src = uncompressed;
	} else {
		src = message;
	}

	protozero::pbf_reader reader(src);

	while (reader.next()) {
		switch (reader.tag()) {
		case 3: /* layer */
		{
			protozero::pbf_reader layer_reader(reader.get_message());
			mvt_layer layer;

			while (layer_reader.next()) {
				switch (layer_reader.tag()) {
				case 1: /* name */
					layer.name = layer_reader.get_string();
					break;

				case 3: /* key */
					layer.keys.push_back(layer_reader.get_string());
					break;

				case 4: /* value */
				{
					protozero::pbf_reader value_reader(layer_reader.get_message());
					mvt_value value;

					while (value_reader.next()) {
						switch (value_reader.tag()) {
						case 1: /* string */
							value = value_reader.get_string();
							break;

						case 2: /* float */
							value = value_reader.get_float();
							break;

						case 3: /* double */
							value = value_reader.get_double();
							break;

						case 4: /* int */
							value = value_reader.get_int64();
							break;

						case 5: /* uint */
							value = value_reader.get_uint64();
							break;

						case 6: /* sint */
							value = value_reader.get_sint64();
							break;

						case 7: /* bool */
							value = (bool) value_reader.get_bool();
							break;

						default:
							value_reader.skip();
							break;
						}
					}

					layer.values.push_back(value);
					break;
				}

				case 5: /* extent */
					layer.extent = layer_reader.get_uint32();
					break;

				case 2: /* feature */
				{
					protozero::pbf_reader feature_reader(layer_reader.get_message());
					mvt_feature feature;
					std::vector<uint32_t> geoms;

					while (feature_reader.next()) {
						switch (feature_reader.tag()) {
						case 2: /* tag */
						{
							auto pi = feature_reader.get_packed_uint32();
							for (auto it = pi.first; it != pi.second; ++it) {
								feature.tags.push_back(*it);
							}
							break;
						}

						case 3: /* feature type */
							feature.type = feature_reader.get_enum();
							break;

						case 4: /* geometry */
						{
							auto pi = feature_reader.get_packed_uint32();
							for (auto it = pi.first; it != pi.second; ++it) {
								geoms.push_back(*it);
							}
							break;
						}

						default:
							feature_reader.skip();
							break;
						}
					}

					long long px = 0, py = 0;
					for (size_t g = 0; g < geoms.size(); g++) {
						uint32_t geom = geoms[g];
						uint32_t op = geom & 7;
						uint32_t count = geom >> 3;

						if (op == mvt_moveto || op == mvt_lineto) {
							for (size_t k = 0; k < count && g + 2 < geoms.size(); k++) {
								px += protozero::decode_zigzag32(geoms[g + 1]);
								py += protozero::decode_zigzag32(geoms[g + 2]);
								g += 2;

								feature.geometry.push_back(mvt_geometry(op, px, py));
							}
						} else {
							feature.geometry.push_back(mvt_geometry(op, 0, 0));
						}
					}

					layer.features.push_back(feature);
					break;
				}

				default:
					layer_reader.skip();
					break;
				}
			}

			for (size_t i = 0; i < layer.keys.size(); i++) {
				layer.key_map.insert(std::pair<std::string, size_t>(layer.keys[i], i));
			}
			for (size_t i = 0; i < layer.values.size(); i++) {
				layer.value_map.insert(std::pair<mvt_value, size_t>(layer.values[i], i));
			}

			layers.push_back(layer);
			break;
		}

		default:
			reader.skip();
			break;
		}
	}

	return true;
}

struct write_visitor {
	protozero::pbf_writer *writer;

	write_visitor(protozero::pbf_writer *w) {
		writer = w;
	}

	void operator()(std::string &val) const {
		writer->add_string(1, val);
	}

	void operator()(float val) const {
		writer->add_float(2, val);
	}

	void operator()(double val) const {
		writer->add_double(3, val);
	}

	void operator()(int64_t val) const {
		writer->add_int64(4, val);
	}

	void operator()(uint64_t val) const {
		writer->add_uint64(5, val);
	}

#if 0  // not defined in the variant
	void operator()(sint64_t val) {
		writer->add_sint64(6, val);
	}
#endif

	void operator()(bool val) const {
		writer->add_bool(6, val);
	}

	void operator()(std::nullptr_t val) const {
		fprintf(stderr, "Can't happen: Null value in tile\n");
		exit(EXIT_FAILURE);
	}

	void operator()(std::vector<mvt_value> &val) const {
		fprintf(stderr, "Can't happen: Vector value in tile\n");
		exit(EXIT_FAILURE);
	}

	void operator()(std::unordered_map<std::string, mvt_value> &val) const {
		fprintf(stderr, "Can't happen: Hash table value in tile\n");
		exit(EXIT_FAILURE);
	}
};

std::string mvt_tile::encode() {
	mapbox::geometry::value va;
	std::string data;

	protozero::pbf_writer writer(data);

	for (size_t i = 0; i < layers.size(); i++) {
		std::string layer_string;
		protozero::pbf_writer layer_writer(layer_string);

		layer_writer.add_uint32(15, layers[i].version); /* version */
		layer_writer.add_string(1, layers[i].name);     /* name */
		layer_writer.add_uint32(5, layers[i].extent);   /* extent */

		for (size_t j = 0; j < layers[i].keys.size(); j++) {
			layer_writer.add_string(3, layers[i].keys[j]); /* key */
		}

		for (size_t v = 0; v < layers[i].values.size(); v++) {
			std::string value_string;
			protozero::pbf_writer value_writer(value_string);

			mvt_value &pbv = layers[i].values[v];
			mapbox::util::apply_visitor(write_visitor(&value_writer), pbv);

			layer_writer.add_message(4, value_string);
		}

		for (size_t f = 0; f < layers[i].features.size(); f++) {
			std::string feature_string;
			protozero::pbf_writer feature_writer(feature_string);

			feature_writer.add_enum(3, layers[i].features[f].type);
			feature_writer.add_packed_uint32(2, std::begin(layers[i].features[f].tags), std::end(layers[i].features[f].tags));

			std::vector<uint32_t> geometry;

			int px = 0, py = 0;
			int cmd_idx = -1;
			int cmd = -1;
			int length = 0;

			std::vector<mvt_geometry> &geom = layers[i].features[f].geometry;

			for (size_t g = 0; g < geom.size(); g++) {
				int op = geom[g].op;

				if (op != cmd) {
					if (cmd_idx >= 0) {
						geometry[cmd_idx] = (length << 3) | (cmd & ((1 << 3) - 1));
					}

					cmd = op;
					length = 0;
					cmd_idx = geometry.size();
					geometry.push_back(0);
				}

				if (op == mvt_moveto || op == mvt_lineto) {
					long long wwx = geom[g].x;
					long long wwy = geom[g].y;

					int dx = wwx - px;
					int dy = wwy - py;

					geometry.push_back(protozero::encode_zigzag32(dx));
					geometry.push_back(protozero::encode_zigzag32(dy));

					px = wwx;
					py = wwy;
					length++;
				} else if (op == mvt_closepath) {
					length++;
				} else {
					fprintf(stderr, "\nInternal error: corrupted geometry\n");
					exit(EXIT_FAILURE);
				}
			}

			if (cmd_idx >= 0) {
				geometry[cmd_idx] = (length << 3) | (cmd & ((1 << 3) - 1));
			}

			feature_writer.add_packed_uint32(4, std::begin(geometry), std::end(geometry));
			layer_writer.add_message(2, feature_string);
		}

		writer.add_message(3, layer_string);
	}

	std::string compressed;
	compress(data, compressed);

	return compressed;
}

void mvt_layer::tag(mvt_feature &feature, std::string key, mvt_value value) {
	size_t ko, vo;

	std::map<std::string, size_t>::iterator ki = key_map.find(key);
	std::map<mvt_value, size_t>::iterator vi = value_map.find(value);

	if (ki == key_map.end()) {
		ko = keys.size();
		keys.push_back(key);
		key_map.insert(std::pair<std::string, size_t>(key, ko));
	} else {
		ko = ki->second;
	}

	if (vi == value_map.end()) {
		vo = values.size();
		values.push_back(value);
		value_map.insert(std::pair<mvt_value, size_t>(value, vo));
	} else {
		vo = vi->second;
	}

	feature.tags.push_back(ko);
	feature.tags.push_back(vo);
}

struct stringify_visitor {
	std::string operator()(std::string const &val) const {
		return std::string("s") + std::string(val);
	}

	std::string operator()(float val) const {
		char *s;
		asprintf(&s, "f%f", val);
		std::string ret(s);
		free(s);
		return ret;
	}

	std::string operator()(double val) const {
		char *s;
		asprintf(&s, "d%lf", val);
		std::string ret(s);
		free(s);
		return ret;
	}

	std::string operator()(int64_t val) const {
		char *s;
		asprintf(&s, "i%lld", (long long) val);
		std::string ret(s);
		free(s);
		return ret;
	}

	std::string operator()(uint64_t val) const {
		char *s;
		asprintf(&s, "u%llu", (unsigned long long) val);
		std::string ret(s);
		free(s);
		return ret;
	}

	std::string operator()(bool val) const {
		return std::string("b") + (val ? std::string("true") : std::string("false"));
	}

	std::string operator()(std::nullptr_t val) const {
		return std::string("n");
	}

	std::string operator()(std::vector<mvt_value> const &val) const {
		return std::string("v");
	}

	std::string operator()(std::unordered_map<std::string, mvt_value> const &val) const {
		return std::string("m");
	}
};

bool mvt_value_cmp::operator()(const mvt_value &a, const mvt_value &b) const {
	std::string as = mapbox::util::apply_visitor(stringify_visitor(), a);
	std::string bs = mapbox::util::apply_visitor(stringify_visitor(), b);

	return as < bs;
}
