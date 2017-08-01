/*
 * Author   : Saurabh Shrivastava
 * Email    : saurabh.shrivastava54@gmail.com
 * Link     : https://github.com/saurabhshri
 */

#include "recognize_using_pocketsphinx.h"

PocketsphinxAligner::PocketsphinxAligner(std::string inputAudioFileName, std::string inputSubtitleFileName)
{
    _audioFileName = inputAudioFileName;
    _subtitleFileName = inputSubtitleFileName;

    LOG("Initialising Aligner using PocketSphinx");
    LOG("Audio Filename : %s Subtitle filename : %s", _audioFileName.c_str(), _subtitleFileName.c_str());

    WaveFileData *file = new WaveFileData(_audioFileName);
    file->read();
    _samples = file->getSamples();

    std::cout << "Reading and processing subtitles..\n";
    SubtitleParserFactory *subParserFactory = new SubtitleParserFactory(_subtitleFileName);
    SubtitleParser *parser = subParserFactory->getParser();
    _subtitles = parser->getSubtitles();
}

bool PocketsphinxAligner::generateGrammar(grammarName name)
{
    LOG("Generating Grammar based on subtitles, Grammar Name : %d ", name);

    std::cout << "Generating Grammar based on subtitles..\n";
    generate(_subtitles, name);
}

bool PocketsphinxAligner::initDecoder(std::string modelPath, std::string lmPath, std::string dictPath, std::string fsgPath, std::string logPath)
{
    LOG("Initialising PocketSphinx decoder");

    _modelPath = modelPath;
    _lmPath = lmPath;
    _dictPath = dictPath;
    _fsgPath = fsgPath;
    _logPath = logPath;

    LOG("Configuration : \n\tmodelPath = %s \n\tlmPath = %s \n\tdictPath = %s \n\tfsgPath = %s \n\tlogPath = %s ",
        _modelPath.c_str(), _lmPath.c_str(), _dictPath.c_str(), _fsgPath.c_str(), _logPath.c_str());

    _config = cmd_ln_init(NULL,
                          ps_args(), TRUE,
                          "-hmm", modelPath.c_str(),
                          "-lm", lmPath.c_str(),
                          "-dict", dictPath.c_str(),
                          "-logfn", logPath.c_str(),
//                          "-cmn", "batch",
//                          "-lw", "1.0",
//                          "-beam", "1e-80",
//                          "-wbeam", "1e-60",
//                          "-pbeam", "1e-80",
                          NULL);

    if (_config == NULL)
    {
        fprintf(stderr, "Failed to create config object, see log for  details\n");
        return -1;
    }

    _ps = ps_init(_config);

    if (_ps == NULL)
    {
        fprintf(stderr, "Failed to create recognizer, see log for  details\n");
        return -1;
    }
}

int levenshtein_distance(const std::string &firstWord, const std::string &secondWord)
{
    const int length1 = firstWord.size();
    const int length2 = secondWord.size();

    std::vector<int> currentColumn(length2 + 1);
    std::vector<int> previousColumn(length2 + 1);

    for (int index2 = 0; index2 < length2 + 1; ++index2)
    {
        previousColumn[index2] = index2;
    }

    for (int index1 = 0; index1 < length1; ++index1)
    {
        currentColumn[0] = index1 + 1;

        for (int index2 = 0; index2 < length2; ++index2)
        {
            const int compare = firstWord[index1] == secondWord[index2] ? 0 : 1;

            currentColumn[index2 + 1] = std::min(std::min(currentColumn[index2] + 1, previousColumn[index2 + 1] + 1), previousColumn[index2] + compare);
        }

        currentColumn.swap(previousColumn);
    }

    return previousColumn[length2];
}

recognisedBlock PocketsphinxAligner::findAndSetWordTimes(cmd_ln_t *config, ps_decoder_t *ps, SubtitleItem *sub)
{
    ps_start_stream(ps);
    int frame_rate = cmd_ln_int32_r(config, "-frate");
    ps_seg_t *iter = ps_seg_iter(ps);

    std::vector<std::string> words = sub->getIndividualWords();

    //converting locally stored words into lowercase - as the recognised words are in lowercase

    for (std::string &eachWord : words)
    {
        std::transform(eachWord.begin(), eachWord.end(), eachWord.begin(), ::tolower);
    }

    int lastWordFoundAtIndex = -1;

    recognisedBlock currentBlock; //storing recognised words and their timing information

    while (iter != NULL)
    {
        int32 sf, ef, pprob;
        float conf;

        ps_seg_frames(iter, &sf, &ef);
        pprob = ps_seg_prob(iter, NULL, NULL, NULL);
        conf = logmath_exp(ps_get_logmath(ps), pprob);

        std::string recognisedWord(ps_seg_word(iter));

        //the time when utterance was marked, the times are w.r.t. to this
        long int startTime = sub->getStartTime();
        long int endTime = startTime;

        /*
         * Finding start time and end time of each word.
         *
         * 1 sec = 1000 ms, thus time in second = 1000 / frame rate.
         *
         */

        startTime += sf * 1000 / frame_rate;
        endTime += ef * 1000 / frame_rate;

        //storing recognised words and their timing information
        currentBlock.recognisedString.push_back(recognisedWord);
        currentBlock.recognisedWordStartTimes.push_back(startTime);
        currentBlock.recognisedWordEndTimes.push_back(endTime);


        /*
         * Suppose this is the case :
         *
         * Actual      : [Why] would you use tomato just why
         * Recognised  : would you use tomato just [why]
         *
         * So, if we search whole recognised sentence for actual words one by one, then Why[1] of Actual will get associated
         * with why[7] of recognised. Thus limiting the number of words it can look ahead.
         *
         */

        int searchWindowSize = 3;

        /*
            Recognised  : so have you can you've brought seven
                               |
                        ---------------
                        |               |
            Actual      : I think you've brought with you

            Recognised  : so have you can you've brought seven
                                    |
                            -------------------
                            |                  |
            Actual      : I think you've brought with you

         */

        //Do not try to search silence and words like [BREATH] et cetera..
        if (recognisedWord == "<s>" || recognisedWord == "</s>" || recognisedWord[0] == '[' || recognisedWord == "<sil>")
            goto skipSearchingThisWord;

        for (int wordIndex = lastWordFoundAtIndex + 1; wordIndex < words.size(); wordIndex++)
        {
            if (wordIndex > (int) currentBlock.recognisedString.size() + searchWindowSize)
                break;

            int distance = levenshtein_distance(words[wordIndex], recognisedWord);
            int largerLength = words[wordIndex].size() > recognisedWord.size() ? words[wordIndex].size() : recognisedWord.size();

            if (distance < largerLength * 0.25)   //at least 75% must match
            {
                std::cout << "Possible Match : " << words[wordIndex];

                lastWordFoundAtIndex = wordIndex;
                sub->setWordRecognisedStatusByIndex(true, wordIndex);
                sub->setWordTimesByIndex(startTime, endTime, wordIndex);

                std::cout << "\t\tStart : \t" << sub->getWordStartTimeByIndex(wordIndex);
                std::cout << "\tEnd : \t" << sub->getWordEndTimeByIndex(wordIndex);
                std::cout << std::endl;

                break;
            }

        }

skipSearchingThisWord:
        iter = ps_seg_next(iter);
    }

    return currentBlock;
}

bool PocketsphinxAligner::printRecognisedWordAsSRT(cmd_ln_t *config, ps_decoder_t *ps)
{
    int frame_rate = cmd_ln_int32_r(config, "-frate");
    ps_seg_t *iter = ps_seg_iter(ps);

    std::ofstream out;
    out.open("output_transcription.srt", std::ofstream::app);

    while (iter != NULL)
    {
        int32 sf, ef, pprob;
        float conf;

        ps_seg_frames(iter, &sf, &ef);
        pprob = ps_seg_prob(iter, NULL, NULL, NULL);
        conf = logmath_exp(ps_get_logmath(ps), pprob);

        std::string recognisedWord(ps_seg_word(iter));
        long startTime = sf * 1000 / frame_rate, endTime = ef * 1000 / frame_rate;

        std::string outputString;
        if (conf < 0.7)
        {
            outputString = "<font color='#FF0000'>";
            outputString += recognisedWord;
            outputString += "</font>\n\n";
        }

        else
        {
            outputString = recognisedWord;
            outputString += "\n\n";
        }

        int hh1, mm1, ss1, ms1;
        int hh2, mm2, ss2, ms2;
        char timeline[128];

        ms_to_srt_time(startTime, &hh1, &mm1, &ss1, &ms1);
        ms_to_srt_time(endTime, &hh2, &mm2, &ss2, &ms2);

        //printing in SRT format
        sprintf(timeline, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\n", hh1, mm1, ss1, ms1, hh2, mm2, ss2, ms2);

        out << timeline;
        out << outputString;

        iter = ps_seg_next(iter);
    }

    out.close();
}

bool PocketsphinxAligner::printWordTimes(cmd_ln_t *config, ps_decoder_t *ps)
{
    ps_start_stream(ps);
    int frame_rate = cmd_ln_int32_r(config, "-frate");
    ps_seg_t *iter = ps_seg_iter(ps);
    while (iter != NULL)
    {
        int32 sf, ef, pprob;
        float conf;

        ps_seg_frames(iter, &sf, &ef);
        pprob = ps_seg_prob(iter, NULL, NULL, NULL);
        conf = logmath_exp(ps_get_logmath(ps), pprob);
        printf(">>> %s \t %.3f \t %.3f\n", ps_seg_word(iter), ((float) sf / frame_rate),
               ((float) ef / frame_rate));
        iter = ps_seg_next(iter);
    }
}

bool PocketsphinxAligner::align(outputOptions printOption)
{
    for (SubtitleItem *sub : _subtitles)
    {
        if (sub->getDialogue().empty())
            continue;


        //first assigning approx timestamps
        currentSub *currSub = new currentSub(sub);
        currSub->run();

        //let's correct the timestamps :)

        long int dialogueStartsAt = sub->getStartTime();
        long int dialogueLastsFor = (sub->getEndTime() - dialogueStartsAt);

        long int samplesAlreadyRead = dialogueStartsAt * 16;
        long int samplesToBeRead = dialogueLastsFor * 16;

        /*
         * 00:00:19,320 --> 00:00:21,056
         * Why are you boring?
         *
         * dialogueStartsAt : 19320 ms
         * dialogueEndsAt   : 21056 ms
         * dialogueLastsFor : 1736 ms
         *
         * SamplesAlreadyRead = 19320 ms * 16 samples/ms = 309120 samples
         * SampleToBeRead     = 1736  ms * 16 samples/ms = 27776 samples
         *
         */

        const int16_t *sample = _samples.data();

        _rv = ps_start_utt(_ps);
        _rv = ps_process_raw(_ps, sample + samplesAlreadyRead, samplesToBeRead, FALSE, FALSE);
        _rv = ps_end_utt(_ps);

        _hyp = ps_get_hyp(_ps, &_score);

        if (_hyp == NULL)
        {
            _hyp = "NULL";
            std::cout << "\n\n-----------------------------------------\n\n";
            std::cout << "Recognised  : " << _hyp << "\n";
            continue;

        }

        std::cout << "\n\n-----------------------------------------\n\n";
        std::cout << "Start time of dialogue : " << dialogueStartsAt << "\n";
        std::cout << "End time of dialogue   : " << sub->getEndTime() << "\n\n";
        std::cout << "Recognised  : " << _hyp << "\n";
        std::cout << "Actual      : " << sub->getDialogue() << "\n\n";

        recognisedBlock currBlock = findAndSetWordTimes(_config, _ps, sub);

        if (printOption == printAsKaraoke || printOption == printAsKaraokeWithDistinctColors)
            currSub->printAsKaraoke("karaoke.srt", printOption);

        else
            currSub->printToSRT("output.srt", printOption);

        delete currSub;
    }
}

bool PocketsphinxAligner::transcribe()
{
    //pointer to samples
    const int16_t *sample = _samples.data();

    bool utt_started, in_speech;

    //creating partition of 2048 bytes

    int numberOfPartitions = _samples.size() / 2048, remainingSamples = _samples.size() % 2048;

    _rv = ps_start_utt(_ps);
    utt_started = FALSE;

    for (int i = 0; i <= numberOfPartitions; i++)
    {
        if (i == numberOfPartitions)
            ps_process_raw(_ps, sample, remainingSamples, FALSE, FALSE);

        else
            ps_process_raw(_ps, sample, 2048, FALSE, FALSE);

        in_speech = ps_get_in_speech(_ps);

        if (in_speech && !utt_started)
        {
            utt_started = TRUE;
        }

        if (!in_speech && utt_started)
        {
            ps_end_utt(_ps);
            _hyp = ps_get_hyp(_ps, NULL);

            if (_hyp != NULL)
            {
                std::cout << "Recognised  : " << _hyp << "\n";
                printRecognisedWordAsSRT(_config, _ps);
            }

            ps_start_utt(_ps);
            utt_started = FALSE;
        }

        sample += 2048;

    }

    _rv = ps_end_utt(_ps);

    if (utt_started)
    {
        _hyp = ps_get_hyp(_ps, NULL);
        if (_hyp != NULL)
        {
            std::cout << "Recognised  : " << _hyp << "\n";
            printRecognisedWordAsSRT(_config, _ps);
        }
    }

}

bool PocketsphinxAligner::reInitDecoder(cmd_ln_t *config, ps_decoder_t *ps)
{
    ps_reinit(_ps, _config);
}

bool PocketsphinxAligner::alignWithFSG()
{
    for (SubtitleItem *sub : _subtitles)
    {
        if (sub->getDialogue().empty())
            continue;


        //first assigning approx timestamps
        currentSub *currSub = new currentSub(sub);
        currSub->run();

        long int dialogueStartsAt = sub->getStartTime();
        std::string fsgname(_fsgPath + std::to_string(dialogueStartsAt));
        fsgname += ".fsg";

        cmd_ln_t *subConfig;
        subConfig = cmd_ln_init(NULL,
                                ps_args(), TRUE,
                                "-hmm", _modelPath.c_str(),
//                                "-lm", _lmPath.c_str(),
                                "-dict", _dictPath.c_str(),
                                "-logfn", _logPath.c_str(),
                                "-fsg", fsgname.c_str(),
//                          "-lw", "1.0",
//                          "-beam", "1e-80",
//                          "-wbeam", "1e-60",
//                          "-pbeam", "1e-80",
                                NULL);

        if (subConfig == NULL)
        {
            fprintf(stderr, "Failed to create config object, see log for  details\n");
            return -1;
        }

        ps_reinit(_ps, subConfig);

        if (_ps == NULL)
        {
            fprintf(stderr, "Failed to create recognizer, see log for  details\n");
            return -1;
        }

        long int dialogueLastsFor = (sub->getEndTime() - dialogueStartsAt);

        long int samplesAlreadyRead = dialogueStartsAt * 16;
        long int samplesToBeRead = dialogueLastsFor * 16;

        const int16_t *sample = _samples.data();

        _rv = ps_start_utt(_ps);
        _rv = ps_process_raw(_ps, sample + samplesAlreadyRead, samplesToBeRead, FALSE, FALSE);
        _rv = ps_end_utt(_ps);

        _hyp = ps_get_hyp(_ps, &_score);

        if (_hyp == NULL)
        {
            _hyp = "NULL";
            std::cout << "\n\n-----------------------------------------\n\n";
            std::cout << "Recognised  : " << _hyp << "\n";
            continue;

        }

        std::cout << "\n\n-----------------------------------------\n\n";
        std::cout << "Start time of dialogue : " << dialogueStartsAt << "\n";
        std::cout << "End time of dialogue   : " << sub->getEndTime() << "\n\n";
        std::cout << "Recognised  : " << _hyp << "\n";
        std::cout << "Actual      : " << sub->getDialogue() << "\n\n";
        recognisedBlock currBlock = findAndSetWordTimes(subConfig, _ps, sub);

        cmd_ln_free_r(subConfig);
        currSub->printToSRT("output_fsg.srt", printBothWithDistinctColors);

        delete currSub;

    }
}

PocketsphinxAligner::~PocketsphinxAligner()
{
    //std::system("rm -rf tempFiles/");
    ps_free(_ps);
    cmd_ln_free_r(_config);
}