#include "tls_syntax.h"

namespace tls {

void
ostream::write_raw(const std::vector<uint8_t>& bytes)
{
  // Not sure what the default argument is here
  // NOLINTNEXTLINE(fuchsia-default-arguments)
  _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
}

// Primitive type writers
ostream&
ostream::write_uint(uint64_t value, int length)
{
  for (int i = length - 1; i >= 0; i -= 1) {
    _buffer.push_back(value >> unsigned(8 * i));
  }
  return *this;
}

ostream&
operator<<(ostream& out, uint8_t data)
{
  return out.write_uint(data, 1);
}

ostream&
operator<<(ostream& out, uint16_t data)
{
  return out.write_uint(data, 2);
}

ostream&
operator<<(ostream& out, uint32_t data)
{
  return out.write_uint(data, 4);
}

ostream&
operator<<(ostream& out, uint64_t data)
{
  return out.write_uint(data, 8);
}

// Because pop_back() on an empty vector is undefined
uint8_t
istream::next()
{
  if (_buffer.empty()) {
    throw ReadError("Attempt to read from empty buffer");
  }

  uint8_t value = _buffer.back();
  _buffer.pop_back();
  return value;
}

// Primitive type readers
template<typename T>
istream&
istream::read_uint(T& data, int length)
{
  uint64_t value = 0;
  for (int i = 0; i < length; i += 1) {
    value = (value << unsigned(8)) + next();
  }
  data = value;
  return *this;
}

istream&
operator>>(istream& in, uint8_t& data)
{
  return in.read_uint(data, 1);
}

istream&
operator>>(istream& in, uint16_t& data)
{
  return in.read_uint(data, 2);
}

istream&
operator>>(istream& in, uint32_t& data)
{
  return in.read_uint(data, 4);
}

istream&
operator>>(istream& in, uint64_t& data)
{
  return in.read_uint(data, 8);
}

} // namespace tls
