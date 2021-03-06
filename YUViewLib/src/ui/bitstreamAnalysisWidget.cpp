/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut f�r Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "bitstreamAnalysisWidget.h"

#include "parser/parserAnnexBAVC.h"
#include "parser/parserAnnexBHEVC.h"
#include "parser/parserAnnexBVVC.h"
#include "parser/parserAnnexBMpeg2.h"
#include "parser/parserAVFormat.h"

#define BITSTREAM_ANALYSIS_WIDGET_DEBUG_OUTPUT 0
#if BITSTREAM_ANALYSIS_WIDGET_DEBUG_OUTPUT
#include <QDebug>
#define DEBUG_ANALYSIS qDebug
#else
#define DEBUG_ANALYSIS(fmt,...) ((void)0)
#endif

using namespace YUView;

BitstreamAnalysisWidget::BitstreamAnalysisWidget(QWidget *parent) :
  QWidget(parent)
{
  ui.setupUi(this);

  ui.streamInfoTreeWidget->setColumnWidth(0, 300);

  updateParsingStatusText(-1);

  connect(ui.showStreamComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &BitstreamAnalysisWidget::showOnlyStreamComboBoxItemChanged);
  connect(ui.colorCodeStreamsCheckBox, &QCheckBox::toggled, this, &BitstreamAnalysisWidget::colorCodeStreamsCheckBoxToggled);
  connect(ui.parseEntireFileCheckBox, &QCheckBox::toggled, this, &BitstreamAnalysisWidget::parseEntireBitstreamCheckBoxToggled);

  currentSelectedItemsChanged(nullptr, nullptr, false);
}

void BitstreamAnalysisWidget::updateParserItemModel()
{
  this->parser->updateNumberModelItems();
  updateParsingStatusText(parser->getParsingProgressPercent());
}

void BitstreamAnalysisWidget::updateStreamInfo()
{
  ui.streamInfoTreeWidget->clear();
  ui.streamInfoTreeWidget->addTopLevelItems(parser->getStreamInfo());
  ui.streamInfoTreeWidget->expandAll();

  if (ui.showStreamComboBox->count() + 1 != int(parser->getNrStreams()))
  {
    ui.showStreamComboBox->clear();
    ui.showStreamComboBox->addItem("Show all streams");
    for (unsigned int i = 0; i < parser->getNrStreams(); i++)
    {
      QString info = parser->getShortStreamDescription(i);
      ui.showStreamComboBox->addItem(QString("Stream %1 - ").arg(i) + info);
    }
  }
}

void BitstreamAnalysisWidget::backgroundParsingDone(QString error)
{
  if (error.isEmpty())
    ui.parsingStatusText->setText("Parsing done.");
  else
    ui.parsingStatusText->setText("Error parsing the file: " + error);
  updateParsingStatusText(100);
}

void BitstreamAnalysisWidget::showOnlyStreamComboBoxItemChanged(int index)
{
  if (!parser)
    return;

  if (showOnlyStream != index - 1)
  {
    showOnlyStream = index - 1;
    parser->setFilterStreamIndex(showOnlyStream);
  }
}

void BitstreamAnalysisWidget::updateParsingStatusText(int progressValue)
{
  if (progressValue <= -1)
    ui.parsingStatusText->setText("No bitstream file selected - Select a bitstream file to start bitstream analysis.");
  else if (progressValue < 100)
    ui.parsingStatusText->setText(QString("Parsing file (%1%)").arg(progressValue));
  else
  {
    const bool parsingLimitSet = !ui.parseEntireFileCheckBox->isChecked();
    ui.parsingStatusText->setText(parsingLimitSet ? "Partial parsing done. Enable full parsing if needed." : "Parsing done.");
  }
}

void BitstreamAnalysisWidget::stopAndDeleteParser()
{
  disconnect(parser.data(), &parserBase::modelDataUpdated, this, &BitstreamAnalysisWidget::updateParserItemModel);
  disconnect(parser.data(), &parserBase::streamInfoUpdated, this, &BitstreamAnalysisWidget::updateStreamInfo);
  disconnect(parser.data(), &parserBase::backgroundParsingDone, this, &BitstreamAnalysisWidget::backgroundParsingDone);

  if (backgroundParserFuture.isRunning())
  {
    DEBUG_ANALYSIS("BitstreamAnalysisWidget::stopAndDeleteParser stopping parser");
    parser->setAbortParsing();
    backgroundParserFuture.waitForFinished();
  }
  parser.reset();
  DEBUG_ANALYSIS("BitstreamAnalysisWidget::stopAndDeleteParser parser stopped and deleted");
}

void BitstreamAnalysisWidget::backgroundParsingFunction()
{
  if (parser)
    parser->runParsingOfFile(compressedFilePath);
}

void BitstreamAnalysisWidget::currentSelectedItemsChanged(playlistItem *item1, playlistItem *item2, bool chageByPlayback)
{
  Q_UNUSED(item2);
  Q_UNUSED(chageByPlayback);

  currentCompressedVideo = dynamic_cast<playlistItemCompressedVideo*>(item1);
  ui.streamInfoTreeWidget->clear();

  const bool isBitstream = !currentCompressedVideo.isNull();
  ui.tabStreamInfo->setEnabled(isBitstream);
  ui.tabPacketAnalysis->setEnabled(isBitstream);
  ui.tabBitrateGraphicsView->setEnabled(isBitstream);

  restartParsingOfCurrentItem();
}

void BitstreamAnalysisWidget::restartParsingOfCurrentItem()
{
  if (!isVisible())
  {
    DEBUG_ANALYSIS("BitstreamAnalysisWidget::restartParsingOfCurrentItem not visible - abort");
    return;
  }

  stopAndDeleteParser();
  
  if (currentCompressedVideo.isNull())
  {
    DEBUG_ANALYSIS("BitstreamAnalysisWidget::restartParsingOfCurrentItem no compressed video - abort");
    updateParsingStatusText(-1);
    ui.streamInfoTreeWidget->clear();
    ui.dataTreeView->setModel(nullptr);
    ui.bitrateBarChart->setModel(nullptr);
    parser.reset();
    return;
  }

  // Setup a new parser
  inputFormat inputFormatType = currentCompressedVideo->getInputFormat();
  if (inputFormatType == inputAnnexBHEVC)
    parser.reset(new parserAnnexBHEVC(this));
  if (inputFormatType == inputAnnexBVVC)
    parser.reset(new parserAnnexBVVC(this));
  else if (inputFormatType == inputAnnexBAVC)
    parser.reset(new parserAnnexBAVC(this));
  else if (inputFormatType == inputLibavformat)
    parser.reset(new parserAVFormat(this));
  parser->enableModel();
  const bool parsingLimitSet = !ui.parseEntireFileCheckBox->isChecked();
  parser->setParsingLimitEnabled(parsingLimitSet);

  connect(parser.data(), &parserBase::modelDataUpdated, this, &BitstreamAnalysisWidget::updateParserItemModel);
  connect(parser.data(), &parserBase::streamInfoUpdated, this, &BitstreamAnalysisWidget::updateStreamInfo);
  connect(parser.data(), &parserBase::backgroundParsingDone, this, &BitstreamAnalysisWidget::backgroundParsingDone);

  compressedFilePath = currentCompressedVideo->getName();

  ui.dataTreeView->setModel(parser->getPacketItemModel());
  ui.dataTreeView->setColumnWidth(0, 600);
  ui.dataTreeView->setColumnWidth(1, 100);
  ui.dataTreeView->setColumnWidth(2, 120);
  ui.bitrateBarChart->setModel(parser->getBitrateItemModel());

  updateStreamInfo();

  // Start the background parsing thread
  updateParsingStatusText(0);
  backgroundParserFuture = QtConcurrent::run(this, &BitstreamAnalysisWidget::backgroundParsingFunction);
  DEBUG_ANALYSIS("BitstreamAnalysisWidget::restartParsingOfCurrentItem new parser created and started");
}

void BitstreamAnalysisWidget::hideEvent(QHideEvent *event)
{
  DEBUG_ANALYSIS("BitstreamAnalysisWidget::hideEvent");
  stopAndDeleteParser();
  QWidget::hideEvent(event);
}

void BitstreamAnalysisWidget::showEvent(QShowEvent *event)
{
  DEBUG_ANALYSIS("BitstreamAnalysisWidget::showEvent");
  restartParsingOfCurrentItem();
  QWidget::showEvent(event);
}
