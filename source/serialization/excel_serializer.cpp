#include <algorithm>
#include <iterator>

#include <xlnt/serialization/excel_serializer.hpp>
#include <xlnt/common/exceptions.hpp>
#include <xlnt/serialization/manifest_serializer.hpp>
#include <xlnt/serialization/relationship_serializer.hpp>
#include <xlnt/serialization/shared_strings_serializer.hpp>
#include <xlnt/serialization/style_serializer.hpp>
#include <xlnt/serialization/theme_serializer.hpp>
#include <xlnt/serialization/workbook_serializer.hpp>
#include <xlnt/serialization/worksheet_serializer.hpp>
#include <xlnt/serialization/xml_document.hpp>
#include <xlnt/serialization/xml_node.hpp>
#include <xlnt/serialization/xml_serializer.hpp>
#include <xlnt/workbook/document_properties.hpp>
#include <xlnt/workbook/manifest.hpp>
#include <xlnt/workbook/workbook.hpp>
#include <xlnt/worksheet/worksheet.hpp>

#include <detail/constants.hpp>

namespace {

std::string::size_type find_string_in_string(const std::string &string, const std::string &substring)
{
    std::string::size_type possible_match_index = string.find(substring.at(0));

    while (possible_match_index != std::string::npos)
    {
        if (string.substr(possible_match_index, substring.size()) == substring)
        {
            return possible_match_index;
        }

        possible_match_index = string.find(substring.at(0), possible_match_index + 1);
    }

    return possible_match_index;
}

bool load_workbook(xlnt::zip_file &archive, bool guess_types, bool data_only, xlnt::workbook &wb)
{
    wb.set_guess_types(guess_types);
    wb.set_data_only(data_only);

    xlnt::manifest_serializer ms(wb.get_manifest());
    ms.read_manifest(xlnt::xml_serializer::deserialize(archive.read(xlnt::constants::ArcContentTypes)));

    if (ms.determine_document_type() != "excel")
    {
        throw xlnt::invalid_file_exception("");
    }

    wb.clear();

    auto workbook_relationships =
        xlnt::relationship_serializer::read_relationships(archive, xlnt::constants::ArcWorkbook);

    for (const auto &relationship : workbook_relationships)
    {
        wb.create_relationship(relationship.get_id(), relationship.get_target_uri(), relationship.get_type());
    }

    auto xml = xlnt::xml_serializer::deserialize(archive.read(xlnt::constants::ArcWorkbook));

    auto root_node = xml.get_child("workbook");

    auto workbook_pr_node = root_node.get_child("workbookPr");
    wb.get_properties().excel_base_date =
        (workbook_pr_node.has_attribute("date1904") && workbook_pr_node.get_attribute("date1904") != "0")
            ? xlnt::calendar::mac_1904
            : xlnt::calendar::windows_1900;

    xlnt::shared_strings_serializer shared_strings_serializer_;
    std::vector<std::string> shared_strings;
    shared_strings_serializer_.read_shared_strings(
        xlnt::xml_serializer::deserialize(archive.read(xlnt::constants::ArcSharedString)), shared_strings);

    for (auto shared_string : shared_strings)
    {
        wb.add_shared_string(shared_string);
    }

    xlnt::style_serializer style_reader_(wb);
    style_reader_.read_stylesheet(xlnt::xml_serializer::deserialize(archive.read(xlnt::constants::ArcStyles)));

    auto sheets_node = root_node.get_child("sheets");

    for (auto sheet_node : sheets_node.get_children())
    {
        auto rel = wb.get_relationship(sheet_node.get_attribute("r:id"));
        auto ws = wb.create_sheet(sheet_node.get_attribute("name"), rel);
        auto ws_filename = "xl/" + rel.get_target_uri();

        xlnt::worksheet_serializer worksheet_serializer(ws);
        worksheet_serializer.read_worksheet(xlnt::xml_serializer::deserialize(archive.read(ws_filename)));
    }

    return true;
}

} // namespace

namespace xlnt {

const std::string excel_serializer::central_directory_signature()
{
    return "\x50\x4b\x05\x06";
}

std::string excel_serializer::repair_central_directory(const std::string &original)
{
    auto pos = find_string_in_string(original, central_directory_signature());

    if (pos != std::string::npos)
    {
        return original.substr(0, pos + 22);
    }

    return original;
}

bool excel_serializer::load_stream_workbook(std::istream &stream, bool guess_types, bool data_only)
{
    std::vector<std::uint8_t> bytes((std::istream_iterator<char>(stream)), std::istream_iterator<char>());

    return load_virtual_workbook(bytes, guess_types, data_only);
}

bool excel_serializer::load_workbook(const std::string &filename, bool guess_types, bool data_only)
{
    try
    {
        archive_.load(filename);
    }
    catch (std::runtime_error)
    {
        throw invalid_file_exception(filename);
    }

    return ::load_workbook(archive_, guess_types, data_only, workbook_);
}

bool excel_serializer::load_virtual_workbook(const std::vector<std::uint8_t> &bytes, bool guess_types, bool data_only)
{
    archive_.load(bytes);

    return ::load_workbook(archive_, guess_types, data_only, workbook_);
}

excel_serializer::excel_serializer(workbook &wb) : workbook_(wb)
{
}

void excel_serializer::write_data(bool /*as_template*/)
{
    relationship_serializer relationship_serializer_;

    relationship_serializer_.write_relationships(workbook_.get_root_relationships(), "", archive_);
    relationship_serializer_.write_relationships(workbook_.get_relationships(), constants::ArcWorkbook, archive_);

    xml_document properties_app_xml;
    workbook_serializer workbook_serializer_(workbook_);
    archive_.writestr(constants::ArcApp, xml_serializer::serialize(workbook_serializer_.write_properties_app()));
    archive_.writestr(constants::ArcCore, xml_serializer::serialize(workbook_serializer_.write_properties_core()));

    theme_serializer theme_serializer_;
    archive_.writestr(constants::ArcTheme, theme_serializer_.write_theme(workbook_.get_loaded_theme()).to_string());

    xlnt::shared_strings_serializer shared_strings_serializer_;
    archive_.writestr(
        constants::ArcSharedString,
        xml_serializer::serialize(shared_strings_serializer_.write_shared_strings(workbook_.get_shared_strings())));

    archive_.writestr(constants::ArcWorkbook, xml_serializer::serialize(workbook_serializer_.write_workbook()));

    style_serializer style_serializer_(workbook_);
    archive_.writestr(constants::ArcStyles, style_serializer_.write_stylesheet().to_string());

    manifest_serializer manifest_serializer_(workbook_.get_manifest());
    archive_.writestr(constants::ArcContentTypes, manifest_serializer_.write_manifest().to_string());

    write_worksheets();
}

void excel_serializer::write_worksheets()
{
    std::size_t index = 0;

    for (auto ws : workbook_)
    {
        for (auto relationship : workbook_.get_relationships())
        {
            if (relationship.get_type() == relationship::type::worksheet &&
                workbook::index_from_ws_filename(relationship.get_target_uri()) == index)
            {
                worksheet_serializer serializer_(ws);
                std::string ws_filename = "xl/" + relationship.get_target_uri();
                archive_.writestr(ws_filename, serializer_.write_worksheet().to_string());
                break;
            }
        }

        index++;
    }
}

void excel_serializer::write_external_links()
{
}

bool excel_serializer::save_stream_workbook(std::ostream &stream, bool as_template)
{
    write_data(as_template);
    archive_.save(stream);

    return true;
}

bool excel_serializer::save_workbook(const std::string &filename, bool as_template)
{
    write_data(as_template);
    archive_.save(filename);

    return true;
}

bool excel_serializer::save_virtual_workbook(std::vector<std::uint8_t> &bytes, bool as_template)
{
    write_data(as_template);
    archive_.save(bytes);

    return true;
}

} // namespace xlnt