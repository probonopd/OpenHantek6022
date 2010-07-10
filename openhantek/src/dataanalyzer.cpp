////////////////////////////////////////////////////////////////////////////////
//
//  OpenHantek
//  dataanalyzer.cpp
//
//  Copyright (C) 2010  Oliver Haag
//  oliver.haag@gmail.com
//
//  This program is free software: you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation, either version 3 of the License, or (at your option)
//  any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program.  If not, see <http://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////////////


#include <cmath>

#include <QColor>
#include <QMutex>

#include <fftw3.h>


#include "dataanalyzer.h"

#include "glscope.h"
#include "helper.h"
#include "settings.h"


////////////////////////////////////////////////////////////////////////////////
// class HorizontalDock
/// \brief Initializes the buffers and other variables.
/// \param settings The target settings object.
/// \param parent The parent widget.
DataAnalyzer::DataAnalyzer(DsoSettings *settings, QObject *parent) : QThread(parent) {
	this->settings = settings;
	
	this->lastBufferSize = 0;
	this->lastWindow = (Dso::WindowFunction) -1;
	this->window = 0;
	
	this->analyzedDataMutex = new QMutex();
}

/// \brief Deallocates the buffers.
DataAnalyzer::~DataAnalyzer() {
	for(int channel = 0; channel < this->analyzedData.count(); channel++) {
		if(this->analyzedData[channel]->samples.voltage.sample)
			delete[] this->analyzedData[channel]->samples.voltage.sample;
		if(this->analyzedData[channel]->samples.spectrum.sample)
			delete[] this->analyzedData[channel]->samples.spectrum.sample;
	}
}

/// \brief Returns the analyzed data.
/// \param channel Channel, whose data should be returned.
/// \return Analyzed data as AnalyzedData struct.
const AnalyzedData *DataAnalyzer::data(int channel) const {
	if(channel < 0 || channel >= this->analyzedData.count())
		return 0;
	
	return this->analyzedData[channel];
}

/// \brief Returns the mutex for the data.
/// \return Mutex for the analyzed data.
QMutex *DataAnalyzer::mutex() const {
	return this->analyzedDataMutex;
}

/// \brief Analyzes the data from the dso.
void DataAnalyzer::run() {
	this->analyzedDataMutex->lock();
	
	// Adapt the number of channels for analyzed data
	for(int channel = this->analyzedData.count(); channel < this->settings->scope.voltage.count(); channel++) {
		this->analyzedData.append(new AnalyzedData);
		this->analyzedData[channel]->samples.voltage.count = 0;
		this->analyzedData[channel]->samples.voltage.interval = 0;
		this->analyzedData[channel]->samples.voltage.sample = 0;
		this->analyzedData[channel]->samples.spectrum.count = 0;
		this->analyzedData[channel]->samples.spectrum.interval = 0;
		this->analyzedData[channel]->samples.spectrum.sample = 0;
		this->analyzedData[channel]->amplitude = 0;
		this->analyzedData[channel]->frequency = 0;
	}
	for(int channel = this->settings->scope.voltage.count(); channel < this->analyzedData.count(); channel++) {
		if(this->analyzedData.last()->samples.voltage.sample)
			delete[] this->analyzedData.last()->samples.voltage.sample;
		if(this->analyzedData.last()->samples.spectrum.sample)
			delete[] this->analyzedData.last()->samples.spectrum.sample;
		this->analyzedData.removeLast();
	}
	
	for(unsigned int channel = 0; channel < (unsigned int) this->analyzedData.count(); channel++) {
		// Check if we got data for this channel or if it's a math channel that can be calculated
		if(((channel < this->settings->scope.physicalChannels) && channel < (unsigned int) this->waitingData.count() && this->waitingData[channel]) || ((channel >= this->settings->scope.physicalChannels) && (this->settings->scope.voltage[channel].used || this->settings->scope.spectrum[channel].used) && this->analyzedData.count() >= 2 && this->analyzedData[0]->samples.voltage.sample && this->analyzedData[1]->samples.voltage.sample)) {
			// Set sampling interval
			this->analyzedData[channel]->samples.voltage.interval = 1.0 / this->waitingDataSamplerate;
			
			unsigned int size;
			if(channel < this->settings->scope.physicalChannels)
				size = this->waitingDataSize[channel];
			else
				size = this->waitingDataSize[0];
			// Reallocate memory for samples if the sample count has changed
			if(this->analyzedData[channel]->samples.voltage.count != size) {
				this->analyzedData[channel]->samples.voltage.count = size;
				if(this->analyzedData[channel]->samples.voltage.sample)
					delete[] this->analyzedData[channel]->samples.voltage.sample;
				this->analyzedData[channel]->samples.voltage.sample = new double[size];
			}
			
			// Physical channels
			if(channel < this->settings->scope.physicalChannels) {
				// Copy the buffer of the oscilloscope into the sample buffer
				if(channel < (unsigned int) this->waitingData.count())
					for(unsigned int position = 0; position < this->waitingDataSize[channel]; position++)
						this->analyzedData[channel]->samples.voltage.sample[position] = this->waitingData[channel][position];
			}
			// Math channel
			else {
				// Set sampling interval
				this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.interval = this->analyzedData[0]->samples.voltage.interval;
				
				// Reallocate memory for samples if the sample count has changed
				if(this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.count != this->analyzedData[0]->samples.voltage.count) {
					this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.count = this->analyzedData[0]->samples.voltage.count;
					if(this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.sample)
						delete[] this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.sample;
					this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.sample = new double[this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.count];
				}
				
				// Calculate values and write them into the sample buffer
				for(unsigned int realPosition = 0; realPosition < this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.count; realPosition++) {
					switch(this->settings->scope.voltage[this->settings->scope.physicalChannels].misc) {
						case Dso::MATHMODE_1ADD2:
							this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.sample[realPosition] = this->analyzedData[0]->samples.voltage.sample[realPosition] + this->analyzedData[1]->samples.voltage.sample[realPosition];
							break;
						case Dso::MATHMODE_1SUB2:
							this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.sample[realPosition] = this->analyzedData[0]->samples.voltage.sample[realPosition] - this->analyzedData[1]->samples.voltage.sample[realPosition];
							break;
						case Dso::MATHMODE_2SUB1:
							this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.sample[realPosition] = this->analyzedData[1]->samples.voltage.sample[realPosition] - this->analyzedData[0]->samples.voltage.sample[realPosition];
							break;
					}
				}
			}
		}
		else {
			// Clear unused channels
			this->analyzedData[channel]->samples.voltage.count = 0;
			this->analyzedData[this->settings->scope.physicalChannels]->samples.voltage.interval = 0;
			if(this->analyzedData[channel]->samples.voltage.sample) {
				delete[] this->analyzedData[channel]->samples.voltage.sample;
				this->analyzedData[channel]->samples.voltage.sample = 0;
			}
		}
	}
	
	this->waitingDataMutex->unlock();
	
	// Lower priority for spectrum calculation
	this->setPriority(QThread::LowPriority);
	
	// Calculate peak-to-peak voltage and frequency
	for(int channel = 0; channel < this->analyzedData.count(); channel++) {
		if(this->settings->scope.voltage[channel].used && this->analyzedData[channel]->samples.voltage.sample) {
			bool aboveTrigger = this->analyzedData[channel]->samples.voltage.sample[0] > this->settings->scope.voltage[channel].trigger;
			double minimalVoltage, maximalVoltage;
			unsigned int firstTrigger = 0, lastTrigger = 0;
			int triggerCount = -1;
			this->analyzedData[channel]->amplitude = 0;
			minimalVoltage = maximalVoltage = this->analyzedData[channel]->samples.voltage.sample[0];
			
			for(unsigned int position = 0; position < this->analyzedData[channel]->samples.voltage.count; position++) {
				// Check trigger condition
				if(aboveTrigger != (this->analyzedData[channel]->samples.voltage.sample[position] > this->settings->scope.voltage[channel].trigger)) {
					aboveTrigger = this->analyzedData[channel]->samples.voltage.sample[position] > this->settings->scope.voltage[channel].trigger;
					// We measure the time between two trigger slopes
					if(aboveTrigger == (this->settings->scope.trigger.slope == Dso::SLOPE_POSITIVE)) {
						triggerCount++;
						if(triggerCount == 0)
							firstTrigger = position;
						else
							this->analyzedData[channel]->amplitude += maximalVoltage - minimalVoltage;
						minimalVoltage = this->analyzedData[channel]->samples.voltage.sample[position];
						maximalVoltage = this->analyzedData[channel]->samples.voltage.sample[position];
						lastTrigger = position;
					}
				}
				else {                          // Get minimal and maximal voltage
					if(this->analyzedData[channel]->samples.voltage.sample[position] < minimalVoltage)
						minimalVoltage = this->analyzedData[channel]->samples.voltage.sample[position];
					else if(this->analyzedData[channel]->samples.voltage.sample[position] > maximalVoltage)
						maximalVoltage = this->analyzedData[channel]->samples.voltage.sample[position];
				}
			}
			
			// Calculate values
			if(triggerCount >= 0) {
				this->analyzedData[channel]->amplitude /= triggerCount;
				this->analyzedData[channel]->frequency = (double) triggerCount / (lastTrigger - firstTrigger) / this->analyzedData[channel]->samples.voltage.interval;
			}
			else {
				this->analyzedData[channel]->amplitude = maximalVoltage - minimalVoltage;
				this->analyzedData[channel]->frequency = 0;
			}
		}
	}
	
	// Calculate spectrums
	for(int channel = 0; channel < this->analyzedData.count(); channel++) {
		if(this->settings->scope.spectrum[channel].used && this->analyzedData[channel]->samples.voltage.sample) {
			// Calculate new window
			if(this->lastWindow != this->settings->scope.spectrumWindow || this->lastBufferSize != this->analyzedData[channel]->samples.voltage.count) {
				if(this->lastBufferSize != this->analyzedData[channel]->samples.voltage.count) {
					this->lastBufferSize = this->analyzedData[channel]->samples.voltage.count;
					
					if(this->window)
						fftw_free(this->window);
					this->window = (double *) fftw_malloc(sizeof(double) * this->lastBufferSize);
				}
				
				unsigned int windowEnd = this->lastBufferSize - 1;
				this->lastWindow = this->settings->scope.spectrumWindow;
				
				switch(this->settings->scope.spectrumWindow) {
					case Dso::WINDOW_HAMMING:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 0.54 - 0.46 * cos(2.0 * M_PI * windowPosition / windowEnd);
						break;
					case Dso::WINDOW_HANN:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 0.5 * (1.0 - cos(2.0 * M_PI * windowPosition / windowEnd));
						break;
					case Dso::WINDOW_COSINE:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = sin(M_PI * windowPosition / windowEnd);
						break;
					case Dso::WINDOW_LANCZOS:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++) {
							double sincParameter = (2.0 * windowPosition / windowEnd - 1.0) * M_PI;
							if(sincParameter == 0)
								*(this->window + windowPosition) = 1;
							else
								*(this->window + windowPosition) = sin(sincParameter) / sincParameter;
						}
						break;
					case Dso::WINDOW_BARTLETT:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 2.0 / windowEnd * (windowEnd / 2 - abs(windowPosition - windowEnd / 2));
						break;
					case Dso::WINDOW_TRIANGULAR:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 2.0 / this->lastBufferSize * (this->lastBufferSize / 2 - abs(windowPosition - windowEnd / 2));
						break;
					case Dso::WINDOW_GAUSS:
						{
							double sigma = 0.4;
							for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
								*(this->window + windowPosition) = exp(-0.5 * pow(((windowPosition - windowEnd / 2) / (sigma * windowEnd / 2)), 2));
						}
						break;
					case Dso::WINDOW_BARTLETTHANN:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 0.62 - 0.48 * abs(windowPosition / windowEnd - 0.5) - 0.38 * cos(2.0 * M_PI * windowPosition / windowEnd);
						break;
					case Dso::WINDOW_BLACKMAN:
						{
							double alpha = 0.16;
							for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
								*(this->window + windowPosition) = (1 - alpha) / 2 - 0.5 * cos(2.0 * M_PI * windowPosition / windowEnd) + alpha / 2 * cos(4.0 * M_PI * windowPosition / windowEnd);
						}
						break;
					//case WINDOW_KAISER:
						// TODO
						//double alpha = 3.0;
						//for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							//*(this->window + windowPosition) = ;
						//break;
					case Dso::WINDOW_NUTTALL:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 0.355768 - 0.487396 * cos(2 * M_PI * windowPosition / windowEnd) + 0.144232 * cos(4 * M_PI * windowPosition / windowEnd) - 0.012604 * cos(6 * M_PI * windowPosition / windowEnd);
						break;
					case Dso::WINDOW_BLACKMANHARRIS:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 0.35875 - 0.48829 * cos(2 * M_PI * windowPosition / windowEnd) + 0.14128 * cos(4 * M_PI * windowPosition / windowEnd) - 0.01168 * cos(6 * M_PI * windowPosition / windowEnd);
						break;
					case Dso::WINDOW_BLACKMANNUTTALL:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 0.3635819 - 0.4891775 * cos(2 * M_PI * windowPosition / windowEnd) + 0.1365995 * cos(4 * M_PI * windowPosition / windowEnd) - 0.0106411 * cos(6 * M_PI * windowPosition / windowEnd);
						break;
					case Dso::WINDOW_FLATTOP:
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 1.0 - 1.93 * cos(2 * M_PI * windowPosition / windowEnd) + 1.29 * cos(4 * M_PI * windowPosition / windowEnd) - 0.388 * cos(6 * M_PI * windowPosition / windowEnd) + 0.032 * cos(8 * M_PI * windowPosition / windowEnd);
						break;
					default: // Dso::WINDOW_RECTANGULAR
						for(unsigned int windowPosition = 0; windowPosition < this->lastBufferSize; windowPosition++)
							*(this->window + windowPosition) = 1.0;
				}
			}
			
			// Set sampling interval
			this->analyzedData[channel]->samples.spectrum.interval = 1.0 / this->analyzedData[channel]->samples.voltage.interval / this->analyzedData[channel]->samples.voltage.count;
			
			// Reallocate memory for samples if the sample count has changed
			if(this->analyzedData[channel]->samples.spectrum.count != this->analyzedData[channel]->samples.voltage.count / 2) {
				this->analyzedData[channel]->samples.spectrum.count = this->analyzedData[channel]->samples.voltage.count / 2;
				if(this->analyzedData[channel]->samples.spectrum.sample)
					delete[] this->analyzedData[channel]->samples.spectrum.sample;
				this->analyzedData[channel]->samples.spectrum.sample = new double[this->analyzedData[channel]->samples.voltage.count];
			}
			
			// Create sample buffer and apply window
			double *windowedValues = new double[this->analyzedData[channel]->samples.voltage.count];
			for(unsigned int position = 0; position < this->analyzedData[channel]->samples.voltage.count; position++)
				windowedValues[position] = this->window[position] * this->analyzedData[channel]->samples.voltage.sample[position];
			
			// Do discrete real to half-complex transformation
			// TODO: Reuse plan and use FFTW_MEASURE to get fastest algorithm
			fftw_plan fftPlan = fftw_plan_r2r_1d(this->analyzedData[channel]->samples.voltage.count, windowedValues, this->analyzedData[channel]->samples.spectrum.sample, FFTW_R2HC, FFTW_ESTIMATE);
			fftw_execute(fftPlan);
			fftw_destroy_plan(fftPlan);
			
			// Deallocate sample buffer
			delete[] windowedValues;
			
			// Convert values into dB (Relative to the reference level)
			double offset = 60 - this->settings->scope.spectrumReference - 20 * log10(sqrt(this->analyzedData[channel]->samples.spectrum.count));
			double offsetLimit = this->settings->scope.spectrumLimit - this->settings->scope.spectrumReference;
			for(unsigned int position = 0; position < this->analyzedData[channel]->samples.spectrum.count; position++) {
				this->analyzedData[channel]->samples.spectrum.sample[position] = 20 * log10(fabs(this->analyzedData[channel]->samples.spectrum.sample[position])) + offset;
				
				// Check if this value has to be limited
				if(offsetLimit > this->analyzedData[channel]->samples.spectrum.sample[position])
					this->analyzedData[channel]->samples.spectrum.sample[position] = offsetLimit;
			}
		}
		else if(this->analyzedData[channel]->samples.spectrum.sample) {
			// Clear unused channels
			this->analyzedData[channel]->samples.spectrum.count = 0;
			this->analyzedData[channel]->samples.spectrum.interval = 0;
			delete[] this->analyzedData[channel]->samples.spectrum.sample;
			this->analyzedData[channel]->samples.spectrum.sample = 0;
		}
	}
	
	this->analyzedDataMutex->unlock();
}

void DataAnalyzer::analyze(const QList<double *> *data, const QList<unsigned int> *size, double samplerate, QMutex *mutex) {
	// Previous analysis still running, drop the new data
	if(this->isRunning())
		return;
	
	// The thread will analyze it, just save the pointers
	mutex->lock();
	this->waitingData.clear();
	this->waitingData.append(*data);
	this->waitingDataSize.clear();
	this->waitingDataSize.append(*size);
	this->waitingDataMutex = mutex;
	this->waitingDataSamplerate = samplerate;
	this->start();
}