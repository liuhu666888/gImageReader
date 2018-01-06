/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * HOCROdtExporter.cc
 * Copyright (C) 2013-2018 Sandro Mani <manisandro@gmail.com>
 *
 * gImageReader is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gImageReader is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "DisplayerToolHOCR.hh"
#include "HOCRDocument.hh"
#include "HOCROdtExporter.hh"
#include "MainWindow.hh"
#include "FileDialogs.hh"
#include "SourceManager.hh"
#include "Utils.hh"

#include <cstring>
#include <iomanip>
#include <libxml++/libxml++.h>
#include <zip.h>

static Glib::ustring manifestNS_URI("urn:oasis:names:tc:opendocument:xmlns:manifest:1.0");
static Glib::ustring manifestNS("manifest");
static Glib::ustring officeNS_URI("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
static Glib::ustring officeNS("office");
static Glib::ustring textNS_URI ("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
static Glib::ustring textNS("text");
static Glib::ustring styleNS_URI ("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
static Glib::ustring styleNS("style");
static Glib::ustring foNS_URI ("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
static Glib::ustring foNS("fo");
static Glib::ustring tableNS_URI ("urn:oasis:names:tc:opendocument:xmlns:table:1.0");
static Glib::ustring tableNS("table");
static Glib::ustring drawNS_URI ("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
static Glib::ustring drawNS("draw");
static Glib::ustring xlinkNS_URI ("http://www.w3.org/1999/xlink");
static Glib::ustring xlinkNS("xlink");
static Glib::ustring svgNS_URI ("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
static Glib::ustring svgNS("svg");

template<typename char_t>
static zip_source* zip_source_buffer_from_data(zip* fzip, const char_t* data, std::size_t len) {
	void* copy = std::malloc(len);
	std::memcpy(copy, data, len);
	return zip_source_buffer(fzip, copy, len, 1);
}

bool HOCROdtExporter::run(const Glib::RefPtr<HOCRDocument>& hocrdocument, std::string& filebasename)
{
	Glib::ustring suggestion = filebasename;
	if(suggestion.empty()) {
		std::vector<Source*> sources = MAIN->getSourceManager()->getSelectedSources();
		suggestion = !sources.empty() ? Utils::split_filename(sources.front()->displayname).first : _("output");
	}

	FileDialogs::FileFilter filter = {_("OpenDocument Text Documents"), {"application/vnd.oasis.opendocument.text"}, {"*.odt"}};
	std::string outname = FileDialogs::save_dialog(_("Save ODT Output..."), suggestion + ".odt", "outputdir", filter);
	if(outname.empty()) {
		return false;
	}
	filebasename = Utils::split_filename(outname).first;

	zip* fzip = zip_open(outname.c_str(), ZIP_CREATE|ZIP_TRUNCATE, nullptr);
	if(!fzip) {
		Utils::message_dialog(Gtk::MESSAGE_WARNING, _("Export failed"), _("The ODT export failed: unable to write output file."));
		return false;
	}
	int pageCount = hocrdocument->pageCount();
	MainWindow::ProgressMonitor monitor(2 * pageCount);
	MAIN->showProgress(&monitor);
	bool success = Utils::busyTask([&] {
		// Image files
		if(zip_dir_add(fzip, "Pictures", ZIP_FL_ENC_UTF_8) < 0) {
			return false;
		}
		std::map<const HOCRItem*, Glib::ustring> imageFiles;
		for(int i = 0; i < pageCount; ++i) {
			monitor.increaseProgress();
			const HOCRPage* page = hocrdocument->page(i);
			if(!page->isEnabled()) {
				continue;
			}
			bool ok = false;
			Utils::runInMainThreadBlocking([&]{ ok = setSource(page->sourceFile(), page->pageNr(), page->resolution(), page->angle()); });
			if(!ok) {
				continue;
			}
			for(const HOCRItem* item : page->children()) {
				writeImage(fzip, imageFiles, item);
			}
		}

		// Mimetype
		{
			Glib::ustring mimetype = "application/vnd.oasis.opendocument.text";
			zip_source* source = zip_source_buffer_from_data(fzip, mimetype.c_str(), mimetype.bytes());
			if(zip_file_add(fzip, "mimetype", source, ZIP_FL_ENC_UTF_8) < 0) {
				zip_source_free(source);
				return false;
			}
		}

		// Manifest
		{
			if(zip_dir_add(fzip, "META-INF", ZIP_FL_ENC_UTF_8) < 0) {
				return false;
			}
			xmlpp::Document doc;
			xmlpp::Element* manifestEl = doc.create_root_node("manifest", manifestNS_URI, manifestNS);
			manifestEl->set_attribute("version", "1.2", manifestNS);

			xmlpp::Element* fileEntryEl = manifestEl->add_child("file-entry", manifestNS);
			fileEntryEl->set_attribute("media-type", "application/vnd.oasis.opendocument.text", manifestNS);
			fileEntryEl->set_attribute("full-path", "/", manifestNS);

			fileEntryEl = manifestEl->add_child("file-entry", manifestNS);
			fileEntryEl->set_attribute("media-type", "text/xml", manifestNS);
			fileEntryEl->set_attribute("full-path", "content.xml", manifestNS);

			for(auto it = imageFiles.begin(), itEnd = imageFiles.end(); it != itEnd; ++it) {
				fileEntryEl = manifestEl->add_child("file-entry", manifestNS);
				fileEntryEl->set_attribute("media-type", "image/png", manifestNS);
				fileEntryEl->set_attribute("full-path", it->second, manifestNS);
			}
			Glib::ustring data = doc.write_to_string();
			zip_source* source = zip_source_buffer_from_data(fzip, data.c_str(), data.bytes());
			if(zip_file_add(fzip, "META-INF/manifest.xml", source, ZIP_FL_ENC_UTF_8) < 0) {
				zip_source_free(source);
				return false;
			}
		}

		// Content
		{
			xmlpp::Document doc;
			xmlpp::Element* documentContentEl = doc.create_root_node("document-content", officeNS_URI, officeNS);
			documentContentEl->set_namespace_declaration(textNS_URI, textNS);
			documentContentEl->set_namespace_declaration(styleNS_URI, styleNS);
			documentContentEl->set_namespace_declaration(foNS_URI, foNS);
			documentContentEl->set_namespace_declaration(drawNS_URI, drawNS);
			documentContentEl->set_namespace_declaration(xlinkNS_URI, xlinkNS);
			documentContentEl->set_namespace_declaration(svgNS_URI, svgNS);
			documentContentEl->set_attribute("version", "1.2", officeNS);

			// - Styles
			xmlpp::Element* automaticStylesEl = documentContentEl->add_child("automatic-styles", officeNS);

			// -- Standard paragraph
			xmlpp::Element* styleEl = automaticStylesEl->add_child("style", styleNS);
			styleEl->set_attribute("name", "P", styleNS);
			styleEl->set_attribute("parent-style-name", "Standard", styleNS);

			// -- Paragraph preceded by page break
			styleEl = automaticStylesEl->add_child("style", styleNS);
			styleEl->set_attribute("name", "PB", styleNS);
			styleEl->set_attribute("family", "paragraph", styleNS);
			styleEl->set_attribute("parent-style-name", "Standard", styleNS);
			xmlpp::Element* paragraphPropertiesEl = styleEl->add_child("paragraph-properties", styleNS);
			paragraphPropertiesEl->set_attribute("break-before", "page", foNS);

			// -- Frame, absolutely positioned on page
			styleEl = automaticStylesEl->add_child("style", styleNS);
			styleEl->set_attribute("name", "F", styleNS);
			styleEl->set_attribute("family", "graphic", styleNS);
			xmlpp::Element* graphicPropertiesEl = styleEl->add_child("graphic-properties", styleNS);
			graphicPropertiesEl->set_attribute("stroke", "none", drawNS);
			graphicPropertiesEl->set_attribute("fill", "none", drawNS);
			graphicPropertiesEl->set_attribute("vertical-pos", "from-top", styleNS);
			graphicPropertiesEl->set_attribute("vertical-rel", "page", styleNS);
			graphicPropertiesEl->set_attribute("horizontal-pos", "from-left", styleNS);
			graphicPropertiesEl->set_attribute("horizontal-rel", "page", styleNS);
			graphicPropertiesEl->set_attribute("flow-with-text", "false", styleNS);

			// -- Text styles
			std::map<Glib::ustring,std::map<double,Glib::ustring>> fontStyles;
			for(int i = 0; i < pageCount; ++i) {
				const HOCRPage* page = hocrdocument->page(i);
				if(page->isEnabled()) {
					for(const HOCRItem* item : page->children()) {
						collectFontStyles(fontStyles, item);
					}
				}
			}
			int counter = 0;
			for(auto ffit = fontStyles.begin(), ffitEnd = fontStyles.end(); ffit != ffitEnd; ++ffit) {
				Glib::ustring fontFamily = ffit->first;
				for(auto fsit = ffit->second.begin(), fsitEnd = ffit->second.end(); fsit != fsitEnd; ++fsit) {
					double fontSize = fsit->first;
					Glib::ustring styleName = Glib::ustring::compose("T%1", ++counter);
					fsit->second = styleName;
					styleEl = automaticStylesEl->add_child("style", styleNS);
					styleEl->set_attribute("name", styleName, styleNS);
					styleEl->set_attribute("family", "text", styleNS);
					xmlpp::Element* textPropertiesEl = styleEl->add_child("text-properties", styleNS);
					textPropertiesEl->set_attribute("font-name", fontFamily, foNS);
					textPropertiesEl->set_attribute("font-size", Glib::ustring::compose("%1pt", fontSize), foNS);
				}
			}

			// - Body
			xmlpp::Element* bodyEl = documentContentEl->add_child("body", officeNS);
			xmlpp::Element* textEl = bodyEl->add_child("text", officeNS);

			bool firstPage = true;
			for(int i = 0; i < pageCount; ++i) {
				monitor.increaseProgress();
				const HOCRPage* page = hocrdocument->page(i);
				if(!page->isEnabled()) {
					continue;
				}
				if(!firstPage) {
					textEl->add_child("p", textNS)->set_attribute("style-name", "PB", textNS);
				}
				firstPage = false;
				for(const HOCRItem* item : page->children()) {
					printItem(textEl, item, page->resolution(), fontStyles, imageFiles);
				}
			}

			Glib::ustring data = doc.write_to_string_formatted();
			zip_source* source = zip_source_buffer_from_data(fzip, data.c_str(), data.bytes());
			if(zip_file_add(fzip, "content.xml", source, ZIP_FL_ENC_UTF_8) < 0) {
				zip_source_free(source);
				return false;
			}
		}
		return true;
	}, _("Exporting to ODT..."));
	MAIN->hideProgress();

	zip_close(fzip);
	if(!success) {
		Utils::message_dialog(Gtk::MESSAGE_WARNING, _("Export failed"), _("The ODT export failed: unable to write output file."));
	}
	return success;
}
#include <fstream>
void HOCROdtExporter::writeImage(zip* fzip, std::map<const HOCRItem*,Glib::ustring>& images, const HOCRItem* item)
{
	if(!item->isEnabled()) {
		return;
	}
	if(item->itemClass() == "ocr_graphic") {
		Cairo::RefPtr<Cairo::ImageSurface> selection;
		Utils::runInMainThreadBlocking([&]{ selection = getSelection(item->bbox()); });
		gchar* guuid = g_uuid_string_random();
		Glib::ustring uuid(guuid);
		uuid = uuid.substr(1, uuid.length() - 2); // Remove {}
		g_free(guuid);
		Glib::ustring filename = Glib::ustring::compose("Pictures/%1.png", uuid);

		Glib::RefPtr<Glib::ByteArray> pngbytes = Glib::ByteArray::create();
		selection->write_to_png_stream([&](const unsigned char* data, unsigned int length)->Cairo::ErrorStatus {
			pngbytes->append(data, length);
			return CAIRO_STATUS_SUCCESS;
		});
		std::ofstream file;
		file.open(filename.c_str());
		file.write(reinterpret_cast<const char*>(pngbytes->get_data()), pngbytes->size());
		file.close();


		zip_source* source = zip_source_buffer_from_data(fzip, pngbytes->get_data(), pngbytes->size());
		if(zip_file_add(fzip, filename.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
			zip_source_free(source);
			return;
		}
		images.insert(std::make_pair(item, filename));
	}
}

void HOCROdtExporter::collectFontStyles(std::map<Glib::ustring,std::map<double,Glib::ustring>>& styles, const HOCRItem* item)
{
	if(!item->isEnabled()) {
		return;
	}
	if(item->itemClass() == "ocrx_word") {
		styles[item->fontFamily()].insert(std::make_pair(item->fontSize(), ""));
	} else {
		for(const HOCRItem* item : item->children()) {
			collectFontStyles(styles, item);
		}
	}
}

void HOCROdtExporter::printItem(xmlpp::Element* parentEl, const HOCRItem* item, int dpi, std::map<Glib::ustring,std::map<double,Glib::ustring>>& fontStyleNames, std::map<const HOCRItem*,Glib::ustring>& images)
{
	if(!item->isEnabled()) {
		return;
	}
	Glib::ustring itemClass = item->itemClass();
	const Geometry::Rectangle& bbox = item->bbox();
	if(itemClass == "ocr_graphic") {
		xmlpp::Element* pEl = parentEl->add_child("p", textNS);
		pEl->set_attribute("style-name", "P", textNS);

		xmlpp::Element* frameEl = pEl->add_child("frame", drawNS);
		frameEl->set_attribute("style-name", "F", drawNS);
		frameEl->set_attribute("anchor-type", "page", textNS);
		frameEl->set_attribute("anchor-page-number", Glib::ustring::format(std::fixed, std::setprecision(0), item->page()->pageNr()), textNS);
		frameEl->set_attribute("x", Glib::ustring::compose("%1in", bbox.x / double(dpi)), svgNS);
		frameEl->set_attribute("y", Glib::ustring::compose("%1in", bbox.y / double(dpi)), svgNS);
		frameEl->set_attribute("width", Glib::ustring::compose("%1in", bbox.width / double(dpi)), svgNS);
		frameEl->set_attribute("height", Glib::ustring::compose("%1in", bbox.height / double(dpi)), svgNS);
		frameEl->set_attribute("z-index", "0", drawNS);

		xmlpp::Element* imageEl = frameEl->add_child("image", drawNS);
		imageEl->set_attribute("href", images[item], xlinkNS);
		imageEl->set_attribute("type", "simple", xlinkNS);
		imageEl->set_attribute("show", "embed", xlinkNS);
	}
	else if(itemClass == "ocr_par") {
		xmlpp::Element* pEl = parentEl->add_child("p", textNS);
		pEl->set_attribute("style-name", "P", textNS);

		xmlpp::Element* frameEl = pEl->add_child("frame", drawNS);
		frameEl->set_attribute("style-name", "F", drawNS);
		frameEl->set_attribute("anchor-type", "page", textNS);
		frameEl->set_attribute("anchor-page-number", Glib::ustring::format(std::fixed, std::setprecision(0), item->page()->pageNr()), textNS);
		frameEl->set_attribute("x", Glib::ustring::compose("%1in", bbox.x / double(dpi)), svgNS);
		frameEl->set_attribute("y", Glib::ustring::compose("%1in", bbox.y / double(dpi)), svgNS);
		frameEl->set_attribute("width", Glib::ustring::compose("%1in", bbox.width / double(dpi)), svgNS);
		frameEl->set_attribute("height", Glib::ustring::compose("%1in", bbox.height / double(dpi)), svgNS);
		frameEl->set_attribute("z-index", "1", drawNS);

		xmlpp::Element* textBoxEl = frameEl->add_child("text-box", drawNS);

		xmlpp::Element* ppEl = textBoxEl->add_child("p", textNS);
		ppEl->set_attribute("style-name", "P", textNS);
		for(const HOCRItem* child : item->children()) {
			printItem(ppEl, child, dpi, fontStyleNames, images);
		}
	} else if(itemClass == "ocr_line") {
		const HOCRItem* firstWord = nullptr;
		int iChild = 0, nChilds = item->children().size();
		for(; iChild < nChilds; ++iChild) {
			const HOCRItem* child = item->children()[iChild];
			if(child->isEnabled()) {
				firstWord = child;
				break;
			}
		}
		if(!firstWord) {
			return;
		}
		Glib::ustring currentFontStyleName = fontStyleNames[firstWord->fontFamily()][firstWord->fontSize()];
		xmlpp::Element* spanEl = parentEl->add_child("span", textNS);
		spanEl->set_attribute("style-name", currentFontStyleName, textNS);
		spanEl->add_child_text(firstWord->text());
		++iChild;
		for(; iChild < nChilds; ++iChild) {
			const HOCRItem* child = item->children()[iChild];
			Glib::ustring fontStyleName = fontStyleNames[child->fontFamily()][child->fontSize()];
			if(fontStyleName != currentFontStyleName) {
				currentFontStyleName = fontStyleName;
				spanEl = parentEl->add_child("span", textNS);
				spanEl->set_attribute("style-name", currentFontStyleName, textNS);
			}
			spanEl->add_child_text(" ");
			spanEl->add_child_text(child->text());
		}
		parentEl->add_child("line-break", textNS);
	} else {
		for(const HOCRItem* child : item->children()) {
			printItem(parentEl, child, dpi, fontStyleNames, images);
		}
	}
}

bool HOCROdtExporter::setSource(const Glib::ustring& sourceFile, int page, int dpi, double angle) {
	Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(sourceFile);
	if(MAIN->getSourceManager()->addSource(file)) {
		MAIN->getDisplayer()->setup(&page, &dpi, &angle);
		return true;
	} else {
		return false;
	}
}

Cairo::RefPtr<Cairo::ImageSurface> HOCROdtExporter::getSelection(const Geometry::Rectangle& bbox) {
	return m_displayerTool->getSelection(bbox);
}