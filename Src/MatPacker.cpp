#include "Baker.hpp"
#include "Ktx2.hpp"

#include <wx/dataview.h>
#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>
#include <wx/wx.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>

#include "ThirdParty/mINI.h"
#include "stb_image.h"

static const char* spcConfigSettingsPath = "config.ini";


/**
 * @brief Persistent application settings, stored as an INI file next to the executable.
 * A missing file or missing keys leave the defaults untouched
 */
class ConfigSettings
{
public:
	ConfigSettings() = default;

	void Load()
	{
		mINI::INIFile file(GetFilePath());
		mINI::INIStructure ini;

		if (!file.read(ini)) {
			return;
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyDefaultOutputDir)) {
			mDefaultOutputDir = ini[spcSectionOutput][spcKeyDefaultOutputDir];
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyCreateSubfolder)) {
			mbCreateSubfolder = ini[spcSectionOutput][spcKeyCreateSubfolder] == "1";
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyResolutionDivisor)) {
			try {
				mResolutionDivisor = std::max(1, std::stoi(ini[spcSectionOutput][spcKeyResolutionDivisor]));
			}
			catch (const std::exception&) {
				// Malformed value: keep the default.
			}
		}
	}

	/// Writes the settings, keeping any other keys already in the file. Returns false on failure.
	bool Save() const
	{
		mINI::INIFile file(GetFilePath());
		mINI::INIStructure ini;

		// Start from the existing file so unrelated keys survive; ignore failure (file may not exist yet).
		file.read(ini);
		ini[spcSectionOutput][spcKeyDefaultOutputDir] = mDefaultOutputDir;
		ini[spcSectionOutput][spcKeyResolutionDivisor] = std::to_string(mResolutionDivisor);
		ini[spcSectionOutput][spcKeyCreateSubfolder] = mbCreateSubfolder ? "1" : "0";

		return file.write(ini, true);
	}

	const std::string& GetDefaultOutputDir() const { return mDefaultOutputDir; }
	void SetDefaultOutputDir(const std::string& dir) { mDefaultOutputDir = dir; }

	int GetResolutionDivisor() const { return mResolutionDivisor; }
	void SetResolutionDivisor(int divisor) { mResolutionDivisor = std::max(1, divisor); }

	bool GetCreateSubfolder() const { return mbCreateSubfolder; }
	void SetCreateSubfolder(bool create) { mbCreateSubfolder = create; }

private:
	static std::string GetFilePath()
	{
		const wxFileName executable(wxStandardPaths::Get().GetExecutablePath());
		return wxFileName(executable.GetPath(), spcConfigSettingsPath).GetFullPath().utf8_string();
	}

	static constexpr const char* spcSectionOutput = "Output";
	static constexpr const char* spcKeyDefaultOutputDir = "DefaultDirectory";
	static constexpr const char* spcKeyResolutionDivisor = "ResolutionDivisor";
	static constexpr const char* spcKeyCreateSubfolder = "CreateSubfolder";

	std::string mDefaultOutputDir;
	int mResolutionDivisor = 1;
	bool mbCreateSubfolder = false;
};

// Shows one image scaled to fit, using nearest-neighbour when enlarging so
// small mips look blocky.
class ImageCanvas : public wxPanel
{
public:
	explicit ImageCanvas(wxWindow* parent) : wxPanel(parent)
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetMinSize(wxSize(320, 240));
		Bind(wxEVT_PAINT, &ImageCanvas::OnPaint, this);
		Bind(wxEVT_SIZE,
			 [this](wxSizeEvent& e)
			 {
				 mbDirty = true;
				 Refresh();
				 e.Skip();
			 });
	}

	void SetImage(const Image8* img)
	{
		mImage = wxImage();

		if (img && !img->Empty()) {
			const size_t n = static_cast<size_t>(img->width) * img->height;
			auto* rgb = static_cast<unsigned char*>(malloc(n * 3));
			auto* alpha = static_cast<unsigned char*>(malloc(n));

			for (size_t i = 0; i < n; ++i) {
				rgb[i * 3] = img->pixels[i * 4];
				rgb[i * 3 + 1] = img->pixels[i * 4 + 1];
				rgb[i * 3 + 2] = img->pixels[i * 4 + 2];
				alpha[i] = img->pixels[i * 4 + 3];
			}

			mImage = wxImage(img->width, img->height, rgb, alpha, false);
		}

		mbDirty = true;
		Refresh();
	}

private:
	void OnPaint(wxPaintEvent&)
	{
		wxAutoBufferedPaintDC dc(this);

		dc.SetBackground(wxBrush(wxColour(48, 48, 48)));
		dc.Clear();

		if (!mImage.IsOk()) {
			return;
		}

		if (mbDirty) {
			const wxSize cs = GetClientSize();
			const double scale = std::min(double(cs.x) / mImage.GetWidth(), double(cs.y) / mImage.GetHeight());
			const int w = std::max(1, int(mImage.GetWidth() * scale));
			const int h = std::max(1, int(mImage.GetHeight() * scale));

			mBitmap = wxBitmap(mImage.Scale(w, h, scale >= 1.0 ? wxIMAGE_QUALITY_NEAREST : wxIMAGE_QUALITY_HIGH));
			mbDirty = false;
		}
		const wxSize cs = GetClientSize();
		dc.DrawBitmap(mBitmap, (cs.x - mBitmap.GetWidth()) / 2, (cs.y - mBitmap.GetHeight()) / 2, true);
	}

private:
	wxImage mImage;
	wxBitmap mBitmap;
	bool mbDirty = true;
};

class MainFrame : public wxFrame
{
public:
	MainFrame() : wxFrame(nullptr, wxID_ANY, "MeatPacker", wxDefaultPosition, wxSize(760, 820))
	{
		mConfig.Load();

		auto* panel = new wxPanel(this);
		auto* root = new wxBoxSizer(wxVERTICAL);

		const wxString image_wildcard = "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm)|"
										"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm|All "
										"files|*.*";

		auto* inputs = new wxStaticBoxSizer(wxVERTICAL, panel, "Input Images");
		wxWindow* ip = inputs->GetStaticBox();

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		wxButton* add_btn = new wxButton(ip, wxID_ANY, "Add Images");
		wxButton* remove_btn = new wxButton(ip, wxID_ANY, "Remove Selected");
		wxButton* clear_btn = new wxButton(ip, wxID_ANY, "Clear");

		buttons->Add(add_btn, 0, wxRIGHT, 6);
		buttons->Add(remove_btn, 0, wxRIGHT, 6);
		buttons->Add(clear_btn, 0);
		inputs->Add(buttons, 0, wxALL, 6);

		mpList = new wxDataViewListCtrl(ip, wxID_ANY, wxDefaultPosition, wxSize(-1, 170), wxDV_MULTIPLE);
		mpList->AppendTextColumn("File", wxDATAVIEW_CELL_INERT, 200);
		wxArrayString role_choices;
		for (const char* r : spcRoles) {
			role_choices.Add(r);
		}
		mpList->AppendColumn(
			new wxDataViewColumn("Role", new wxDataViewChoiceRenderer(role_choices, wxDATAVIEW_CELL_EDITABLE), 1, 170),
			"string");

		mpList->AppendTextColumn("Size", wxDATAVIEW_CELL_INERT, 90);
		mpList->AppendTextColumn("Folder", wxDATAVIEW_CELL_INERT, 300);
		inputs->Add(mpList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
		root->Add(inputs, 0, wxEXPAND | wxALL, 8);

		add_btn->Bind(wxEVT_BUTTON, &MainFrame::OnAddImages, this);
		remove_btn->Bind(wxEVT_BUTTON, &MainFrame::OnRemoveSelected, this);
		clear_btn->Bind(wxEVT_BUTTON,
						[this](wxCommandEvent&)
						{
							mpList->DeleteAllItems();
							mPaths.clear();
						});
		mpList->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, &MainFrame::OnRoleChanged, this);

		auto* defs = new wxStaticBoxSizer(wxVERTICAL, panel, "Fallback values");
		auto* dgrid = new wxFlexGridSizer(2, 6, 8);

		auto AddSpinner = [&](const wxString& label, double init, wxSpinCtrlDouble*& out)
		{
			dgrid->Add(new wxStaticText(defs->GetStaticBox(), wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
			out = new wxSpinCtrlDouble(defs->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
									   wxSP_ARROW_KEYS, 0.0, 1.0, init, 0.05);
			out->SetDigits(3);
			dgrid->Add(out);
		};

		AddSpinner("Roughness", 0.5, mpDefRoughness);
		AddSpinner("Metallic", 0.0, mpDefMetallic);
		AddSpinner("AO", 1.0, mpDefAmbientOcclusion);

		defs->Add(dgrid, 0, wxALL, 6);
		root->Add(defs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto* out = new wxStaticBoxSizer(wxVERTICAL, panel, "Output");
		auto* ogrid = new wxFlexGridSizer(2, 6, 8);
		ogrid->AddGrowableCol(1);
		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Directory"), 0, wxALIGN_CENTER_VERTICAL);
		auto* dir_row = new wxBoxSizer(wxHORIZONTAL);
		mpOutDir = new wxTextCtrl(out->GetStaticBox(), wxID_ANY, wxString::FromUTF8(mConfig.GetDefaultOutputDir()));
		auto* dir_browse = new wxButton(out->GetStaticBox(), wxID_ANY, "Browse");
		auto* dir_set_default = new wxButton(out->GetStaticBox(), wxID_ANY, "Set as default");

		dir_browse->Bind(wxEVT_BUTTON,
						 [this](wxCommandEvent&)
						 {
							 wxDirDialog dlg(this, "Select output directory", mpOutDir->GetValue(),
											 wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
							 if (dlg.ShowModal() == wxID_OK)
								 mpOutDir->SetValue(dlg.GetPath());
						 });

		dir_set_default->Bind(wxEVT_BUTTON,
							  [this](wxCommandEvent&)
							  {
								  mConfig.SetDefaultOutputDir(mpOutDir->GetValue().utf8_string());

								  if (mConfig.Save()) {
									  mpLog->AppendText("Default output directory saved.\n");
								  }
								  else {
									  mpLog->AppendText("Error: could not save the default output directory.\n");
								  }
							  });

		dir_row->Add(mpOutDir, 1, wxEXPAND | wxRIGHT, 6);
		dir_row->Add(dir_browse, 0, wxRIGHT, 6);
		dir_row->Add(dir_set_default, 0);

		ogrid->Add(dir_row, 1, wxEXPAND);
		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Base Name"), 0, wxALIGN_CENTER_VERTICAL);

		mpName = new wxTextCtrl(out->GetStaticBox(), wxID_ANY, "material");
		ogrid->Add(mpName, 1, wxEXPAND);

		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Resolution"), 0, wxALIGN_CENTER_VERTICAL);

		mpDivisor = new wxChoice(out->GetStaticBox(), wxID_ANY);
		for (const char* label : spcDivisorLabels) {
			mpDivisor->Append(label);
		}

		// Restore the saved divisor; fall back to full size if it is not one of the offered values.
		int divisor_index = 0;
		for (int i = 0; i < int(std::size(spcDivisors)); ++i) {
			if (spcDivisors[i] == mConfig.GetResolutionDivisor()) {
				divisor_index = i;
			}
		}

		mpDivisor->SetSelection(divisor_index);

		mpDivisor->Bind(wxEVT_CHOICE,
						[this](wxCommandEvent&)
						{
							mConfig.SetResolutionDivisor(spcDivisors[std::max(0, mpDivisor->GetSelection())]);

							if (!mConfig.Save()) {
								mpLog->AppendText("Error: could not save the resolution setting.\n");
							}
						});

		ogrid->Add(mpDivisor, 0);
		out->Add(ogrid, 0, wxEXPAND | wxALL, 6);

		mpMips = new wxCheckBox(out->GetStaticBox(), wxID_ANY, "Generate Mipmaps");
		mpMips->SetValue(true);

		out->Add(mpMips, 0, wxLEFT | wxBOTTOM, 8);

		mpSubfolder = new wxCheckBox(out->GetStaticBox(), wxID_ANY, "Create Subfolder");

		mpSubfolder->SetValue(mConfig.GetCreateSubfolder());
		mpSubfolder->Bind(wxEVT_CHECKBOX,
						  [this](wxCommandEvent&)
						  {
							  mConfig.SetCreateSubfolder(mpSubfolder->GetValue());

							  if (!mConfig.Save()) {
								  mpLog->AppendText("Error: could not save the subfolder setting.\n");
							  }
						  });

		out->Add(mpSubfolder, 0, wxLEFT | wxBOTTOM, 8);
		root->Add(out, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto* bake = new wxButton(panel, wxID_ANY, "Bake");
		root->Add(bake, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		mpTabs = new wxNotebook(panel, wxID_ANY);
		mpLog = new wxTextCtrl(mpTabs, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
		mpTabs->AddPage(mpLog, "Log");

		auto* viewer = new wxPanel(mpTabs);
		auto* vsizer = new wxBoxSizer(wxHORIZONTAL);
		auto* left = new wxBoxSizer(wxVERTICAL);

		mpTexChoice = new wxChoice(viewer, wxID_ANY);
		mpLevelList = new wxListBox(viewer, wxID_ANY, wxDefaultPosition, wxSize(200, -1));
		mpLevelInfo = new wxStaticText(viewer, wxID_ANY, "Bake to view the mip levels.");

		left->Add(mpTexChoice, 0, wxEXPAND | wxBOTTOM, 6);
		left->Add(mpLevelList, 1, wxEXPAND | wxBOTTOM, 6);
		left->Add(mpLevelInfo, 0, wxEXPAND);

		mpCanvas = new ImageCanvas(viewer);
		vsizer->Add(left, 0, wxEXPAND | wxALL, 6);
		vsizer->Add(mpCanvas, 1, wxEXPAND | wxALL, 6);
		viewer->SetSizer(vsizer);
		mpTabs->AddPage(viewer, "Mip viewer", true);

		root->Add(mpTabs, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		mpTexChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ShowTexture(mpTexChoice->GetSelection()); });
		mpLevelList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowLevel(mpLevelList->GetSelection()); });

		panel->SetSizer(root);
		bake->Bind(wxEVT_BUTTON, &MainFrame::OnBake, this);
	}

private:
	enum class eImageRole
	{
		Unused,
		Diffuse,
		Normal,
		Roughness,
		Metallic,
		AmbientOcclusion
	};

	// Output resolution divisors offered in the UI; labels and values must stay in the same order.
	static constexpr int spcDivisors[] = { 1, 2, 4, 8 };
	static constexpr const char* spcDivisorLabels[] = { "Full size", "1/2 (half)", "1/4 (quarter)", "1/8 (eighth)" };

	static constexpr const char* spcRoles[] = { "Unused",			"Diffuse / Base colour",
												"Normal map",		"Roughness (ORM.G)",
												"Metallic (ORM.B)", "AO (ORM.R)" };

	eImageRole RoleOfRow(int row) const
	{
		wxString text = mpList->GetTextValue(row, 1);

		for (int i = 0; i < 6; ++i) {
			if (text == spcRoles[i]) {
				return eImageRole(i);
			}
		}

		return eImageRole::Unused;
	}

	int RowWithRole(eImageRole role, int except = -1) const
	{
		for (int row = 0; row < int(mPaths.size()); ++row) {
			if (row != except && RoleOfRow(row) == role) {
				return row;
			}
		}
		return -1;
	}

	// Reads just the image header, so this is cheap even for large files.
	static wxString GetImageSizeText(const wxString& path)
	{
		int width = 0, height = 0, channels = 0;

		if (!stbi_info(path.utf8_str(), &width, &height, &channels)) {
			return "unreadable";
		}

		return wxString::Format("%d x %d", width, height);
	}

	// Guesses a role from common texture naming ("rock_Normal.png", "wall_ao.jpg").
	static eImageRole GuessRole(const wxString& filename)
	{
		wxString stem = wxFileName(filename).GetName().Lower();
		wxString spaced = stem;

		for (size_t i = 0; i < spaced.length(); ++i) {
			if (!wxIsalnum(spaced[i])) {
				spaced[i] = ' ';
			}
		}

		wxArrayString tokens = wxSplit(spaced, ' ');

		auto FileNameContains = [&](std::initializer_list<const char*> words)
		{
			for (const char* w : words) {
				if (stem.Contains(w) || tokens.Index(w) != wxNOT_FOUND) {
					return true;
				}
			}

			return false;
		};

		if (FileNameContains({ "normal", "nrm", "nor" })) {
			return eImageRole::Normal;
		}
		if (FileNameContains({ "rough", "rgh" })) {
			return eImageRole::Roughness;
		}
		if (FileNameContains({ "metal", "mtl" })) {
			return eImageRole::Metallic;
		}
		if (tokens.Index("ao") != wxNOT_FOUND || FileNameContains({ "occlusion", "ambient" })) {
			return eImageRole::AmbientOcclusion;
		}
		if (FileNameContains({ "diffuse", "albedo", "basecolor", "base", "color", "diff", "col" })) {
			return eImageRole::Diffuse;
		}

		return eImageRole::Unused;
	}

	/**
	 * @brief Find a common base name to generate a default value for the material prefix. Returns nothin if there is no
	 * common name.
	 */
	std::optional<wxString> FindCommonBaseName(const wxArrayString& paths) const
	{
		std::optional<wxString> common;

		for (const wxString& path : paths) {
			const wxString stem = wxFileName(path).GetName();

			// Find last underscore
			const int split = stem.Find('_', true);

			if (split <= 0) {
				continue;
			}

			const wxString prefix = stem.Left(split);

			if (prefix.Length() > 1) {
				return std::make_optional<wxString>(prefix);
			}
		}

		return std::nullopt;
	}

	/**
	 * @brief Adds images to the image pool. Auto assigns them roles
	 */
	void OnAddImages(wxCommandEvent&)
	{
		const wxString wildcard = "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm)|"
								  "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm|All "
								  "files|*.*";

		wxFileDialog dialog(this, "Select images", mLastDir, "", wildcard,
							wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);

		if (dialog.ShowModal() != wxID_OK) {
			return;
		}

		wxArrayString paths;
		dialog.GetPaths(paths);

		for (const wxString& path : paths) {
			if (mPaths.Index(path) != wxNOT_FOUND) {
				continue;
			}

			mLastDir = wxFileName(path).GetPath();

			// Only auto-assign a guessed role if nothing already has that role
			eImageRole role = GuessRole(path);
			if (role != eImageRole::Unused && RowWithRole(role) >= 0) {
				role = eImageRole::Unused;
			}

			wxVector<wxVariant> row;

			row.push_back(wxFileName(path).GetFullName());
			row.push_back(wxString(spcRoles[int(role)]));
			row.push_back(GetImageSizeText(path));
			row.push_back(wxFileName(path).GetPath());

			mpList->AppendItem(row);
			mPaths.Add(path);
		}

		auto base_name = FindCommonBaseName(mPaths);

		if (base_name.has_value()) {
			mpName->SetValue(base_name.value());
		}
	}

	void OnRemoveSelected(wxCommandEvent&)
	{
		wxDataViewItemArray items;
		mpList->GetSelections(items);
		std::vector<int> rows;

		for (const wxDataViewItem& item : items) {
			rows.push_back(mpList->ItemToRow(item));
		}

		std::sort(rows.rbegin(), rows.rend());

		for (int row : rows) {
			mpList->DeleteItem(row);
			mPaths.RemoveAt(row);
		}
	}

	// Each role holds one image; giving it to a new row frees it on the old one.
	void OnRoleChanged(wxDataViewEvent& event)
	{
		int row = mpList->ItemToRow(event.GetItem());

		if (row == wxNOT_FOUND || event.GetColumn() != 1) {
			return;
		}

		eImageRole role = RoleOfRow(row);

		if (role == eImageRole::Unused) {
			return;
		}

		for (int other; (other = RowWithRole(role, row)) >= 0;) {
			mpList->SetTextValue(spcRoles[static_cast<int>(eImageRole::Unused)], other, 1);
		}
	}

	void OnBake(wxCommandEvent&)
	{
		BakeSettings bake_settings;

		for (int row = 0; row < int(mPaths.size()); row++) {
			std::string path = mPaths[row].ToStdString();
			switch (RoleOfRow(row)) {
			case eImageRole::Diffuse:
				bake_settings.PathDiffuse = path;
				break;
			case eImageRole::Normal:
				bake_settings.PathNormalMap = path;
				break;
			case eImageRole::Roughness:
				bake_settings.PathRoughness = path;
				break;
			case eImageRole::Metallic:
				bake_settings.PathMetallic = path;
				break;
			case eImageRole::AmbientOcclusion:
				bake_settings.PathAO = path;
				break;
			case eImageRole::Unused:
				break;
			}
		}

		bake_settings.DefaultRoughness = float(mpDefRoughness->GetValue());
		bake_settings.DefaultMetallic = float(mpDefMetallic->GetValue());
		bake_settings.DefaultAO = float(mpDefAmbientOcclusion->GetValue());
		bake_settings.OutputDir = mpOutDir->GetValue().ToStdString();
		bake_settings.BaseName = mpName->GetValue().ToStdString();
		bake_settings.bExportMipmaps = mpMips->GetValue();
		bake_settings.bCreateSubfolder = mpSubfolder->GetValue();
		bake_settings.ResolutionDivisor = spcDivisors[std::max(0, mpDivisor->GetSelection())];

		mpLog->Clear();
		wxBusyCursor busy;
		std::vector<std::string> written;

		bool ok = Bake(
			bake_settings,
			[this](const std::string& line)
			{
				mpLog->AppendText(wxString::FromUTF8(line) + "\n");
				wxYield();
			},
			&written);

		mpLog->AppendText(ok ? "Done.\n" : "Finished with errors.\n");

		LoadViewer(written);

		if (ok && !written.empty()) {
			mpTabs->SetSelection(1);
		}
	}

	// Reads the baked files back so the viewer shows exactly what is on disk.
	void LoadViewer(const std::vector<std::string>& files)
	{
		mTextures.clear();
		mpTexChoice->Clear();

		for (const std::string& path : files) {
			std::vector<Image8> levels;
			std::string e = ReadKtx2Levels(path, levels);

			if (!e.empty()) {
				mpLog->AppendText("Viewer: " + wxString::FromUTF8(e) + "\n");
				continue;
			}

			mTextures.push_back(std::move(levels));
			mpTexChoice->Append(wxFileName(wxString::FromUTF8(path)).GetFullName());
		}

		if (mTextures.empty()) {
			mpLevelList->Clear();
			mpCanvas->SetImage(nullptr);
			mpLevelInfo->SetLabel("No textures to show.");
			return;
		}

		mpTexChoice->SetSelection(0);
		ShowTexture(0);
	}

	void ShowTexture(int index)
	{
		mpLevelList->Clear();

		if (index < 0 || index >= int(mTextures.size())) {
			return;
		}

		const auto& levels = mTextures[index];

		for (size_t i = 0; i < levels.size(); i++) {
			mpLevelList->Append(wxString::Format("Level %zu  -  %d x %d", i, levels[i].width, levels[i].height));
		}

		mpLevelList->SetSelection(0);
		ShowLevel(0);
	}

	void ShowLevel(int level)
	{
		const int tex = mpTexChoice->GetSelection();

		if (tex < 0 || level < 0 || level >= int(mTextures[tex].size())) {
			return;
		}

		const Image8& img = mTextures[tex][level];
		mpCanvas->SetImage(&img);
		mpLevelInfo->SetLabel(wxString::Format("%d x %d  (%zu bytes)", img.width, img.height, img.pixels.size()));
	}

private:
	wxDataViewListCtrl* mpList;
	wxArrayString mPaths; // full path per list row
	wxString mLastDir;
	wxSpinCtrlDouble *mpDefRoughness, *mpDefMetallic, *mpDefAmbientOcclusion;
	wxTextCtrl *mpOutDir, *mpName, *mpLog;
	wxCheckBox* mpMips;
	wxCheckBox* mpSubfolder;
	wxChoice* mpDivisor;
	wxNotebook* mpTabs;
	wxChoice* mpTexChoice;
	wxListBox* mpLevelList;
	wxStaticText* mpLevelInfo;
	ImageCanvas* mpCanvas;

	std::vector<std::vector<Image8>> mTextures;
	ConfigSettings mConfig;
};

class App : public wxApp
{
public:
	bool OnInit() override
	{
		SetAppearance(Appearance::Dark);
		(new MainFrame)->Show();
		return true;
	}
};

wxIMPLEMENT_APP(App);
