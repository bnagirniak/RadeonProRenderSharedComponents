/*********************************************************************************************************************************
* Radeon ProRender for plugins
* Copyright (c) 2017 AMD
* All Rights Reserved
*
* IES file parser
*********************************************************************************************************************************/

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iterator>
#include <algorithm>
#include <memory>
#include <iomanip>
#include <map>
#include <functional>
#include <clocale>
#include <cmath>
#include "IESprocessor.h"

const char* IESProcessor::IES_FileTag = "IESNA";

IESProcessor::IESLightData::IESLightData()
{
}

void IESProcessor::IESLightData::Clear()
{
	m_countLamps = 0;
	m_lumens = 0.0f;
	m_multiplier = 0.0f;
	m_countVerticalAngles = 0;
	m_countHorizontalAngles = 0;
	m_photometricType = 0;
	m_unit = 0;
	m_width = 0.0f;
	m_length = 0.0f;
	m_height = 0.0f;
	m_ballast = 0;
	m_version = 0;
	m_wattage = -1.0f;
	m_verticalAngles.clear();
	m_horizontalAngles.clear();
	m_candelaValues.clear();
	m_extraData.clear();
}

bool IESProcessor::IESLightData::IsValid() const
{
	// check values correctness (some params could have only one or two values, according to specification)
	bool areValuesCorrect =
		(m_countLamps >= 1) &&  // Should be >= 1 according to IES specification
		((m_lumens == -1) || (m_lumens > 0)) &&
		(m_photometricType == 1) &&
		((m_unit == 1) || (m_unit == 2)) &&
		(m_ballast == 1) &&
		(m_version == 1) &&
		(m_wattage >= 0.0f); // while Autodesk specification says it always should be zero, this is not the case with real files
	if (!areValuesCorrect)
		return false;

	// check table correctness
	bool isSizeCorrect = ( (m_horizontalAngles.size() * m_verticalAngles.size()) == m_candelaValues.size() );
	if (!isSizeCorrect)
		return false;

	// compare stored array size values with real array sizes (they should match)
	bool isArrDataConsistent = (m_horizontalAngles.size() == m_countHorizontalAngles) && (m_verticalAngles.size() == m_countVerticalAngles);
	if (!isArrDataConsistent)
		return false;

	// check data correctness (both angles arrays should be in ascending order)
	bool areArrsSorted = std::is_sorted(m_horizontalAngles.begin(), m_horizontalAngles.end()) &&
		std::is_sorted(m_verticalAngles.begin(), m_verticalAngles.end());
	if (!areArrsSorted)
		return false;

	// ensure correct value for angles
	bool correctAngles = IsAxiallySymmetric() || IsQuadrantSymmetric() || IsPlaneSymmetric() || IsAsymmetric();
	if (!correctAngles)
		return false;

	return true;
}

// the distribution is axially symmetric.
bool IESProcessor::IESLightData::IsAxiallySymmetric(void) const
{
	return (abs(m_horizontalAngles.back()) <= FLT_EPSILON);
}

// the distribution is symmetric in each quadrant.
bool IESProcessor::IESLightData::IsQuadrantSymmetric(void) const
{
	return (abs(m_horizontalAngles.back() - 90.0f) <= FLT_EPSILON);
}

// the distribution is symmetric about a vertical plane.
bool IESProcessor::IESLightData::IsPlaneSymmetric(void) const
{
	return (abs(m_horizontalAngles.back() - 180.0f) <= FLT_EPSILON);
}

bool IESProcessor::IESLightData::IsAsymmetric(void) const
{
	return (abs(m_horizontalAngles.back() - 360.0f) <= FLT_EPSILON);
}

std::string IESProcessor::ToString(const IESLightData& lightData) const
{
	std::stringstream stream(lightData.m_extraData);

	// Write IES header
	stream << lightData.m_extraData;

	// add first line of IES format
	stream
		<< lightData.m_countLamps << ' '
		<< lightData.m_lumens << ' '
		<< lightData.m_multiplier << ' '
		<< lightData.m_countVerticalAngles << ' '
		<< lightData.m_countHorizontalAngles << ' '
		<< lightData.m_photometricType << ' '
		<< lightData.m_unit << ' '
		<< (float)lightData.m_width << ' '
		<< (float)lightData.m_length << ' '
		<< (float)lightData.m_height << std::endl;

	// add second line of IES format
	stream
		<< lightData.m_ballast << ' '
		<< lightData.m_version << ' '
		<< (float)lightData.m_wattage << std::endl;

	// add third line of IES format
	for (double angle : lightData.m_verticalAngles)
	{
		stream << std::to_string(angle) << ' ';
	}

	stream << std::endl;

	// add forth line of IES format
	for (double angle : lightData.m_horizontalAngles)
	{
		stream << std::to_string(angle) << ' ';
	}

	stream << std::endl;

	// verticle angles count is number of columns in candela values table
	size_t valuesPerLine = lightData.m_verticalAngles.size();
	size_t indexInLine = 0;

	for (double candelaValue : lightData.m_candelaValues)
	{
		stream << std::to_string(candelaValue);

		// Put the end of the line where need
		if (++indexInLine == valuesPerLine)
		{
			stream << std::endl;
			indexInLine = 0;
		}
		else
		{
			stream << ' ';
		}
	}

	return stream.str();
}

void IESProcessor::SplitLine(std::vector<std::string>& tokens, const std::string& lineToParse) const
{
	// split string
	std::istringstream iss(lineToParse); // will be able to separate string to substrings dividing them by spaces
	std::vector<std::string> line_tokens
	(
		std::istream_iterator<std::string>(iss), {} // iterator iterates through sub strings separated by space values
	);

	// add splited strings 
	tokens.insert(tokens.end(), line_tokens.begin(), line_tokens.end());
}

// if line starts not with a number after space character then this is a line with extra data
// if line has spaces and the numbers than this is a line with data to be parsed
bool LineHaveNumbers(const std::string& lineToParse)
{
	const char* c = lineToParse.c_str();
	while (c && isspace((unsigned char)*c))
	{
		++c;
	}

	return (isdigit(*c) || (*c == '-'));
}

IESProcessor::ErrorCode IESProcessor::GetTokensFromFile(std::vector<std::string>& tokens, std::string& text, std::ifstream& inputFile) const
{
	text.clear();

	std::string lineToParse;

	// file is not IES file => return
	if (!std::getline(inputFile, lineToParse))
	{
		return IESProcessor::ErrorCode::NOT_IES_FILE;
	}
	
	text += lineToParse += "\n";

	if (std::strncmp(text.c_str(), IES_FileTag, 5) != 0)
	{
		return IESProcessor::ErrorCode::NOT_IES_FILE;
	}

	// parse file line after line
	while (std::getline(inputFile, lineToParse))
	{
		// skip all data irrelevant for render
		if (!LineHaveNumbers(lineToParse))
		{
			text += lineToParse += "\n";
			continue;
		}

		// split line
		SplitLine(tokens, lineToParse);
	}

	return IESProcessor::ErrorCode::SUCCESS;
}

bool ReadDouble(const std::string& input, double& output)
{
	char* pEnd;
	const char* pStr = input.c_str();
	output = strtod(pStr, &pEnd);
	return (pStr != pEnd);
}

bool ReadInt(const std::string& input, int& output)
{
	int Base = 10;
	char* pEnd;
	const char* pStr = input.c_str();
	output = strtol(pStr, &pEnd, Base);
	return (pStr != pEnd);
}

enum class IESProcessor::ParseState
{
	READ_COUNT_LAMPS = 0,
	READ_LUMENS,
	READ_MULTIPLIER,
	READ_COUNT_VANGLES,
	READ_COUNT_HANGLES,
	READ_TYPE,
	READ_UNIT,
	READ_WIDTH,
	READ_LENGTH,
	READ_HEIGHT,
	READ_BALLAST,
	READ_VERSION,
	READ_WATTAGE,
	READ_VERTICAL_ANGLES,
	READ_HORIZONTAL_ANGLES,
	READ_CANDELA_VALUES,
	END_OF_PARSE,
	PARSE_FAILED
};

IESProcessor::ParseState IESProcessor::FirstParseState(void) const
{
	return ParseState::READ_COUNT_LAMPS;
}

IESProcessor::ParseState ReadCountLamps(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_countLamps = data;
	return IESProcessor::ParseState::READ_LUMENS;
}

IESProcessor::ParseState ReadLumens(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_lumens = data;
	return IESProcessor::ParseState::READ_MULTIPLIER;
}

IESProcessor::ParseState ReadMultiplier(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_multiplier = data;
	return IESProcessor::ParseState::READ_COUNT_VANGLES;
}

IESProcessor::ParseState ReadCountVAngles(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_countVerticalAngles = data;
	return IESProcessor::ParseState::READ_COUNT_HANGLES;
}

IESProcessor::ParseState ReadCountHAngles(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_countHorizontalAngles = data;
	return IESProcessor::ParseState::READ_TYPE;
}

IESProcessor::ParseState ReadType(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_photometricType = data;
	return IESProcessor::ParseState::READ_UNIT;
}

IESProcessor::ParseState ReadUnit(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_unit = data;
	return IESProcessor::ParseState::READ_WIDTH;
}

IESProcessor::ParseState ReadWidth(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_width = data;
	return IESProcessor::ParseState::READ_LENGTH;
}

IESProcessor::ParseState ReadLength(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_length = data;
	return IESProcessor::ParseState::READ_HEIGHT;
}

IESProcessor::ParseState ReadHeight(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_height = data;
	return IESProcessor::ParseState::READ_BALLAST;
}

IESProcessor::ParseState ReadBallast(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_ballast = data;
	return IESProcessor::ParseState::READ_VERSION;
}

IESProcessor::ParseState ReadVersion(IESProcessor::IESLightData& lightData, const std::string& value)
{
	int data;
	if (!ReadInt(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_version = data;
	return IESProcessor::ParseState::READ_WATTAGE;
}

IESProcessor::ParseState ReadWattage(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_wattage = data;
	return IESProcessor::ParseState::READ_VERTICAL_ANGLES;
}

IESProcessor::ParseState ReadVAngles(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_verticalAngles.push_back(data);
	if (lightData.m_verticalAngles.size() == lightData.m_countVerticalAngles)
		return IESProcessor::ParseState::READ_HORIZONTAL_ANGLES;

	return IESProcessor::ParseState::READ_VERTICAL_ANGLES; // exit function without switching state because we haven't read all angle values yet
}

IESProcessor::ParseState ReadHAngles(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_horizontalAngles.push_back(data);
	if (lightData.m_horizontalAngles.size() == lightData.m_countHorizontalAngles)
		return IESProcessor::ParseState::READ_CANDELA_VALUES;

	return IESProcessor::ParseState::READ_HORIZONTAL_ANGLES; // exit function without switching state because we haven't read all angle values yet
}

IESProcessor::ParseState ReadCValues(IESProcessor::IESLightData& lightData, const std::string& value)
{
	double data;
	if (!ReadDouble(value, data))
		return IESProcessor::ParseState::PARSE_FAILED;

	lightData.m_candelaValues.push_back(data);
	if (lightData.m_candelaValues.size() == lightData.m_countVerticalAngles*lightData.m_countHorizontalAngles)
		return IESProcessor::ParseState::END_OF_PARSE;

	return IESProcessor::ParseState::READ_CANDELA_VALUES; // exit function without switching state because we haven't read all candela values yet
}

bool IESProcessor::ReadValue(IESLightData& lightData, IESProcessor::ParseState& state, const std::string& value) const
{
	typedef std::function<IESProcessor::ParseState(IESProcessor::IESLightData&, const std::string&)> parseFunc;
	static const std::map<IESProcessor::ParseState, parseFunc > m_parseImpl = {
		{ IESProcessor::ParseState::READ_COUNT_LAMPS,         parseFunc(ReadCountLamps)},
		{ IESProcessor::ParseState::READ_LUMENS,              parseFunc(ReadLumens)},
		{ IESProcessor::ParseState::READ_MULTIPLIER,          parseFunc(ReadMultiplier)},
		{ IESProcessor::ParseState::READ_COUNT_VANGLES,       parseFunc(ReadCountVAngles)},
		{ IESProcessor::ParseState::READ_COUNT_HANGLES,       parseFunc(ReadCountHAngles)},
		{ IESProcessor::ParseState::READ_TYPE,                parseFunc(ReadType)},
		{ IESProcessor::ParseState::READ_UNIT,                parseFunc(ReadUnit)},
		{ IESProcessor::ParseState::READ_WIDTH,               parseFunc(ReadWidth)},
		{ IESProcessor::ParseState::READ_LENGTH,              parseFunc(ReadLength)},
		{ IESProcessor::ParseState::READ_HEIGHT,              parseFunc(ReadHeight)},
		{ IESProcessor::ParseState::READ_BALLAST,             parseFunc(ReadBallast)},
		{ IESProcessor::ParseState::READ_VERSION,             parseFunc(ReadVersion)},
		{ IESProcessor::ParseState::READ_WATTAGE,             parseFunc(ReadWattage)},
		{ IESProcessor::ParseState::READ_VERTICAL_ANGLES,     parseFunc(ReadVAngles)},
		{ IESProcessor::ParseState::READ_HORIZONTAL_ANGLES,   parseFunc(ReadHAngles)},
		{ IESProcessor::ParseState::READ_CANDELA_VALUES,      parseFunc(ReadCValues)},
	};

	// back-off
	if (state == ParseState::END_OF_PARSE)
		return false;

	if (state == ParseState::PARSE_FAILED)
		return false;

	// read values from input
	const auto& parseFuncImpl = m_parseImpl.find(state);
	if (parseFuncImpl != m_parseImpl.end())
	{
		state = parseFuncImpl->second(lightData, value);
		return true;
	}

	return false;
}

IESProcessor::ErrorCode IESProcessor::ParseTokens(IESLightData& lightData, std::vector<std::string>& tokens) const
{
	// initial state to read data
	IESProcessor::ParseState parseState = FirstParseState();

	// iterate over tokens
	for (const std::string& value : tokens)
	{
		// try parse token
		if (!ReadValue(lightData, parseState, value))
		{
			// parse failed
			return IESProcessor::ErrorCode::PARSE_FAILED;
		}
	}

	// parse is not complete => failure
	if (parseState != ParseState::END_OF_PARSE)
	{
		return IESProcessor::ErrorCode::UNEXPECTED_END_OF_FILE;
	}

	// everything seems good
	return IESProcessor::ErrorCode::SUCCESS;
}

IESProcessor::ErrorCode IESProcessor::Parse(IESLightData& lightData, const wchar_t* filename) const
{
#if defined(__MACOSX)
	// Todo : std::ifstream does not take a wchar_t filename on OSX
	return IESProcessor::ErrorCode::NO_FILE;
#else
	// back-off
	if (filename == nullptr)
	{
		return IESProcessor::ErrorCode::NO_FILE;
	}

	// try open file
	std::ifstream inputFile(filename);
	if (!inputFile)
	{
		return IESProcessor::ErrorCode::FAILED_TO_READ_FILE;
	}

	// flush data that might exist in container
	lightData.Clear();

	// read data from file in a way convinient for further parsing
	std::vector<std::string> tokens;
	IESProcessor::ErrorCode fileRead = GetTokensFromFile(tokens, lightData.m_extraData, inputFile);
	if (fileRead != IESProcessor::ErrorCode::SUCCESS)
	{
		// report failure
		lightData.Clear(); // function shouldn't return garbage
		return fileRead;
	}

	// read tokens to lightData
	char* currLocale = std::setlocale(LC_NUMERIC, "");
	std::setlocale(LC_NUMERIC, "en-US");

	IESProcessor::ErrorCode isParseOk = ParseTokens(lightData, tokens);

	std::setlocale(LC_NUMERIC, currLocale);

	// ensure correct parse results
	if ((isParseOk != IESProcessor::ErrorCode::SUCCESS) || !lightData.IsValid())
	{
		// report failure
		lightData.Clear(); // function shouldn't return garbage
		return isParseOk;
	}

	// parse successfull!
	return IESProcessor::ErrorCode::SUCCESS;
#endif
}

IESProcessor::ErrorCode IESProcessor::Update(IESLightData& lightData, const IESUpdateRequest& req) const
{
	// scale photometric web
	if (std::fabs(req.m_scale - 1.0f) > 0.01f)
	{
		lightData.m_width *= req.m_scale;
		lightData.m_length *= req.m_scale;
		lightData.m_height *= req.m_scale;
	}

	return IESProcessor::ErrorCode::SUCCESS;
}

